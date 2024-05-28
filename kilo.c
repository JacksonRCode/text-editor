/*** Includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** Defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    // set to high value so they don't interfere with any char vals
    ARROW_LEFT = 1000,
    ARROW_RIGHT, // 1001
    ARROW_UP, // 1002
    ARROW_DOWN, // 1003
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER, 
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** Data ***/

struct editorSyntax {
    // Name of the filetype that will be displayed to the user.
    char *filetype;
    /*array of strings where each string contains a pattern
    // to match a filename against.
    // If a filename matches then the file will be recognized as 
    // having that filetype
    */
    char **filematch;
    char **keywords;
    // bit field that will contain flags for highlighting
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

typedef struct erow {
    int idx;
    int size;
    int rSize; //render size
    char *chars;
    char *render;
    unsigned char *hl; // highlight
    // Each value in hl array will correspond to a character to render
    // It will tell you whether the character is part of a string, or a comment, or a num...
    int hl_open_comment;
} erow;

struct editorConfig {
    // Gloal struct containing editor state
    int cx, cy;
    int rx; //index into the render field
    int rowOff; // row that user has scrolled to
    int colOff; //col user has scrolled to
    int deadSnap;
    int screenRows;
    int screenCols;
    int numRows;
    erow *row;
    int dirty;
    char *fileName;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
    struct termios orig_termios;
};

struct editorConfig E;

/*** Filetypes ***/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
char *C_HL_keywords[] = {
  "switch", "if", "while", "for", "break", "continue", "return", "else",
  "struct", "union", "typedef", "static", "enum", "class", "case",
  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|", NULL
};
struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** Prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** Terminal ***/

void die(const char *s) {
    /*
    Print error message, clear screen, and exit
    */
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode(void) {
    /*
    orig_termios is the original terminal settings that are
    reapplied when the program exits
    */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) 
        die("tcsetattr");
}

void enableRawMode(void) {
    // Read current terminal attributes into struct orig_termios
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    
    // atexit() is called automatically when the program exits
    atexit(disableRawMode);

    // Copy terminal attributes to raw and then modify them
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= ~(CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
        
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey(void) {
    /*
    Reads single bit at a time unless invalid and return it.
    */
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        char seq[3];

        // Read the next two bytes into the seq buffer
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                // page up and down are esc[~5 and esc[~6 which is what we are checking for
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    // escape sequence 6n retrieves & sends cursor pos. to stdout

    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        // We look for R because the cursor position is stored like:
        // xx;yyR where xx are row nums and yy are col nums
        if (buf[i] == 'R') break;
        i++;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    // parses two integers separated by a ; and put the values
    // into rows and cols variables
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // This line moves cursor right 999 (B) 
        // Also moves cursor down 999 (C)
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** Syntax Highlighting***/

int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
    row->hl = realloc(row->hl, row->size);
    memset(row->hl, HL_NORMAL, row->size);

    if (E.syntax == NULL) return;

    char **keywords = E.syntax->keywords;

    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while (i < row->rSize) {
        char c = row->render[i];;
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rSize - 1);
                break;
            }
        }

        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_COMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rSize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;

                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen])) {
                memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                i += klen;
                break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }


        prev_sep = is_separator(c); 
        i++; 
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numRows) {
        editorUpdateSyntax(&E.row[row->idx + 1]);
    }

}

int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 32;
        case HL_STRING: return 35;
        case HL_MATCH: return 34;
        case HL_KEYWORD1: return 33;
        case HL_KEYWORD2: return 36;
        case HL_NUMBER: return 31;
        default: return 37;
    }
}

void editorSelectSyntaxHighlight(void) {
    E.syntax = NULL;
    if (E.fileName == NULL) return;

    char *ext = strrchr(E.fileName , '.');

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                    (!is_ext && strstr(E.fileName, s->filematch[i]))) {
                E.syntax = s;
                
                int filerow;
                for (filerow = 0; filerow < E.numRows; filerow++) {
                    /* Loop through each row in the file and call
                       editorUpdateSyntax() on it to each highlighting
                       as soon as the filetype changes.
                    */
                    editorUpdateSyntax(&E.row[filerow]);
                }
                
                return; 
            }
            i++;
        }
    }
}

/*** Row Operations ***/

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }

    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        cur_rx++;

        if (cur_rx > rx) return cx;
    }

    return cx;
}

void editorUpdateRow(erow *row) {
    /*
    Characters in chars are transferred to and then formatted
    in render. Render takes inputs and puts them into a universal
    format on all OS.
    */
    
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    // Tabs will take max of 8 spaces, and they already take 1
    // So multiply # of tabs by 7 to allocate correct amount of memory
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

    int i = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[i++] = ' ';
            // Add spaces until you hit a column divisible by 8 (tab stop)
            while (i % KILO_TAB_STOP != 0) row->render[i++] = ' ';
        } else {
            row->render[i++] = row->chars[j];
        }
    }
    row->render[i] = '\0';
    row->rSize = i;

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numRows) return;

    // Update rows
    E.row = realloc(E.row, sizeof(erow) * (E.numRows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * E.numRows - at);
    for (int j = at + 1; j <= E.numRows; j++) E.row[j].idx++;

    E.row[at].idx = at;

    E.row[at].size = len;
    E.row[at].chars = malloc(len+1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rSize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);

    E.numRows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numRows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numRows - at - 1));
    for (int j = at; j < E.numRows - 1; j++) E.row[j].idx--;
    E.numRows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    // at is the index we want to insert character at
    if (at < 0 || at > row->size) at = row->size;
    // add 2 to make room for the null byte
    row->chars = realloc(row->chars, row->size + 2);
    // Copy row->size - at + 1 bytes
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    // memcpy copies s onto end of current row
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDeleteCharacter(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** Editor Operations ***/

void editorInsertChar(int c) {
    if (E.cy == E.numRows) {
        editorInsertRow(E.numRows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewLine(void) {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDeleteChar(void) {
    if (E.cy == E.numRows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDeleteCharacter(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy-1].size;
        // Prev row is 1st arg, curr row is 2nd and 3rd
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** File I/O ***/

char *editorRowsToString(int *bufLen) {
    // Converts row of erow structs into a single string 
    // that is ready to be written out to a file
    int totLen = 0;
    int j;
    for (j = 0; j < E.numRows; j++) {
        // Add 1 for '\n'
        totLen += E.row[j].size + 1;
    }
    *bufLen = totLen;

    char *buf = malloc(totLen);
    char *p = buf;
    for (j = 0; j < E.numRows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }   

    return buf;
}

void editorOpen(char *filename) {
    free(E.fileName);
    E.fileName = strdup(filename);

    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t lineCap = 0;
    ssize_t lineLen;
    while ((lineLen = getline(&line, &lineCap, fp)) != -1) {
        while (lineLen > 0 && (line[lineLen - 1] == '\n' ||
                               line[lineLen - 1] == '\r'))
            lineLen--;
        
        editorInsertRow(E.numRows, line, lineLen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave(void) {
    if (E.fileName == NULL) {
        E.fileName = editorPrompt("Save as: %s", NULL);
        if (E.fileName == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);

    // Open file to read/write or create file with 0644 permissions (r/w)
    int fd = open(E.fileName, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O erro: %s", strerror(errno));
}

/*** Find ***/

void editorFindCallback(char *query, int key) {
    static int last_match = -1;
    static int direction = -1;

    static int saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rSize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }
    if (last_match == -1) direction = 1;
    int current = last_match;

    int i;
    for (i = 0; i < E.numRows; i++) {
        current += direction;
        if (current == -1) current = E.numRows - 1;
        else if (current == E.numRows) current = 0;


        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowOff = E.numRows;


            saved_hl_line = current;
            saved_hl = malloc(row->rSize);
            memcpy(saved_hl, row->hl, row->rSize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind(void) {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_colOff = E.colOff;
    int saved_rowOff = E.rowOff;

    char *query = editorPrompt("Search: %s (ESC/Arrows/Enter)", editorFindCallback);
    
    if (query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.rowOff = saved_rowOff;
        E.colOff = saved_colOff;
    }
}


/*** Append Buffer ***/

struct abuf {
    // b is an array not a character jeez
    char *b;
    int len;
};

// represents empty buffer and acts as constructor for abuf type
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    // make sure to allocate enough memory to hold old str + new str
    // char [] is an array you dummy jeez
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** Output ***/

void editorScroll(void) {
    E.rx = 0;
    if (E.cy < E.numRows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    // Row offset is at the beginning of the screen
    if (E.cy < E.rowOff) {
        // This shifts screen as cursor moves up
        E.rowOff = E.cy;
    }
    if (E.cy >= E.rowOff + E.screenRows) {
        // This shifts screen as cursor moves down
        E.rowOff = E.cy - E.screenRows + 1;
    }
    if (E.rx < E.colOff) {
        // Scrolling to left
        E.colOff = E.rx;
    }
    if (E.rx >= E.colOff + E.screenCols) {
        // Scrolling to right
        E.colOff = E.rx - E.screenCols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenRows; y++) {
        int fileRow = y + E.rowOff;
        if (fileRow >= E.numRows) {  
            // Only display welcome message when buffer is empty
            if (E.numRows == 0 && y == E.screenRows / 3) {
                char welcome[80];
                int welcomeLen = snprintf(welcome, sizeof(welcome),
                    "Kilo editor -- version %s", KILO_VERSION);
                if (welcomeLen > E.screenCols) welcomeLen = E.screenCols;
                int padding = (E.screenCols - welcomeLen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomeLen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[fileRow].rSize - E.colOff;
            if (len < 0) len = 0;
            if (len > E.screenCols) len = E.screenCols;
            char *c = &E.row[fileRow].render[E.colOff];
            unsigned char *hl = &E.row[fileRow].hl[E.colOff];
            int current_color = -1;
            int j;
            for (j=0; j < len; j++) {
                if (iscntrl(c[j])) {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "/x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {                        
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) { 
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab,"\x1b[39m", 5); 
        }
        // K erases current line with no argument
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);        
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    // Escape sequence 7m for za inverted colors ya??
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rStatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.fileName ? E.fileName : "[No Name]", E.numRows,
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rStatus, sizeof(rStatus), "%s | %d/%d",
        E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numRows);
    if (len > E.screenCols) len = E.screenCols;
    abAppend(ab, status, len);
    while (len < E.screenCols) {
        if (E.screenCols - len == rlen) {
            abAppend(ab, rStatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    // Use za m to svitch back to za normal colors ya??
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    // esc sequence K clears message bar
    abAppend(ab, "\x1b[K", 3);
    int msgLen = strlen(E.statusmsg);
    if (msgLen > E.screenCols) msgLen = E.screenCols;
    // Only display the message if it is less than 5 seconds old
    if (msgLen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msgLen);
}

void editorRefreshScreen(void) {
    editorScroll();
    
    struct abuf ab = ABUF_INIT;

    //clear screen then set cursor to top left
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowOff) + 1,
                                              (E.rx - E.colOff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    // ... make this a variadic function
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    // passing NULL to time gets current time
    E.statusmsg_time = time(NULL);
}

/*** Input Helper ***/

void snapCursorX(void) {
    erow *row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];
    if (row && E.deadSnap < row->size) {
        E.cx = E.deadSnap;
    } else if (row) {
        E.cx = row->size;
    }
}

/*** Input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
            // make sure c isn't a special character
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback) callback(buf, c);
    }
}

void editorMoveCursor(int key) {
    // Get current row
    erow *row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy != 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            E.deadSnap = E.cx;

            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            E.deadSnap = E.cx;

            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
                snapCursorX();
            }
            break;
        case ARROW_DOWN:
            if (E.cy != E.numRows) {
                E.cy++;
                snapCursorX();
            }
            break;
    }
}

void editorProcessKeypress(void) {
    /*
    Handles keypresses.
    */
    static int quit_times = KILO_QUIT_TIMES;
    
    int c = editorReadKey();

    switch (c) {
        // Enter
        case '\r':
            editorInsertNewLine();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                "Press Ctrl-Q %d more times to quit.", quit_times);
            quit_times--;
            return;
            }

            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            if (E.cy < E.numRows)
                E.cx = E.row[E.cy].size;
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDeleteChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowOff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowOff + E.screenRows - 1;
                    if (E.cy > E.numRows) E.cy = E.numRows;
                }


                int times = E.screenRows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = KILO_QUIT_TIMES;
}

/*** Init ***/

void initEditor(void) {
    /*
    Initializes all fields in the E struct.
    */
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.numRows = 0;
    E.rowOff = 0; // scrolled to top by default
    E.colOff = 0; // scrolled to start by default
    E.deadSnap = 0; // Holds column value when moving cursor to line without text
    E.row = NULL;
    E.dirty = 0;
    E.fileName = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;

    if (getWindowSize(&E.screenRows, &E.screenCols) == -1) die("getWindowSize");
    E.screenRows -= 2;
    //sd
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage(
        "HELP: Ctrl-S = save | Ctrl-Q = quit | CTRL-F = find");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
