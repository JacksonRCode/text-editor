/*** Includes ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** Defines ***/

/*
Macros transform youdfr program before compilation
#define creates macros -- this is a variable macro
with a bitwise and operation that strips bit 5 and 6 from
whatever key is pressed in combination with ctrl
*/
#define CTRL_KEY(k) ((k) & 0x1f)

/*** Data ***/

struct termios orig_termios;

/*** Terminal ***/

void die(const char *s) {
    /*
    Print error message and exit
    */
    perror(s);
    exit(1);
}

void disableRawMode(void) {
    /*
    orig_termios is the original terminal settings that are
    reapplied when the program exits
    */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) 
        die("tcsetattr");
}

void enableRawMode(void) {
    // Read current terminal attributes into struct orig_termios
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    
    // atexit() is called automatically when the program exits
    atexit(disableRawMode);

    // Copy terminal attributes to raw and then modify them
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= ~(CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
        
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

char editorReadKey(void) {
    /*
    Reads single bit at a time unless invalid and return it.
    */
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    return c;
}

void editorProcessKeypress(void) {
    /*
    Handles keypresses.
    */
    char c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            exit(0);
            break;
    }
}

/*** Output ***/

void editorRefreshScreen() {
    /*
    The 4 in write means we are writing 4 bytes out to the terminal

    1st byte is \x1b which is the escape character or 27 in decimal

    [2J are the other 3 bytes

    We are writing an escape sequence to the terminal.
    - Escape sequences always start with an escape character 27 
      followed by a [ character. Escape sequences instruct the terminal to do
      various text formatting tasks, such as coloring text, moving cursor,
      and clearing parts of screen.

    - J command clears the screen, the # before ca specifies part of screen
      to clear:
      - 0 = cursor and beyond
      - 1 = up to cursor
      - 2 = whole screen
    */
    write(STDOUT_FILENO, "\x1b[2J", 4);
    /*
    3 byte escape sequence

    H command positions the cursor at row 1 col 1 by default
    */
    write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** Input ***/

/*** Init ***/

int main(void) {
    editorRefreshScreen();
    enableRawMode();
    
    while (1) {
        editorProcessKeypress();
    }

    return 0;
}
