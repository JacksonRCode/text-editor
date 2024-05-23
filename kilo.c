/*** Includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** Defines ***/

#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
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

/*** Data ***/

typedef struct erow {
    int size;
    char *chars;
} erow;

struct editorConfig {
    // Gloal struct containing editor state
    int cx, cy;
    int screenRows;
    int screenCols;
    int numRows;
    erow *row;
    struct termios orig_termios;
};

struct editorConfig E;

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

/*** Row Operations ***/

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numRows + 1));

    int at = E.numRows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len+1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numRows++;;
}

/*** File I/O ***/

void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t lineCap = 0;
    ssize_t lineLen;
    lineLen = getline(&line, &lineCap, fp);
    while ((lineLen = getline(&line, &lineCap, fp)) != -1) {
        while (lineLen > 0 && (line[lineLen - 1] == '\n' ||
                               line[lineLen - 1] == '\r'))
            lineLen--;
        
        editorAppendRow(line, lineLen);
    }
    free(line);
    fclose(fp);
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

void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenRows; y++) {
        if (y >= E.numRows) {  
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
            int len = E.row[y].size;
            if (len > E.screenCols) len = E.screenCols;
            abAppend(ab, E.row[y].chars, len);
        }
        // K erases current line with no argument
        abAppend(ab, "\x1b[K", 3);

        // Makes sure a newline isn't written on the last line, 
        // resulting in a line with no tilde.
        if (y < E.screenRows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen(void) {
    struct abuf ab = ABUF_INIT;

    //clear screen then set cursor to top left
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy+1, E.cx+1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** Input ***/

void editorMoveCursor(int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) E.cx--;
            break;
        case ARROW_RIGHT:
            if (E.cx != E.screenCols) E.cx++;
            break;
        case ARROW_UP:
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy != E.screenRows) E.cy++;
            break;
    }
}

void editorProcessKeypress(void) {
    /*
    Handles keypresses.
    */
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;

        case END_KEY:
            E.cx = E.screenCols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
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

    }
}

/*** Init ***/

void initEditor(void) {
    /*
    Initializes all fields in the E struct.
    */
    E.cx = 0;
    E.cy = 0;
    E.numRows = 0;
    E.row = NULL;

    if (getWindowSize(&E.screenRows, &E.screenCols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }
    
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
