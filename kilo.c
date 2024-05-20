#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

// This will store the original state of the terminal
struct termios orig_termios;

void disableRawMode(void) {
    /*
    orig_termios is the original terminal settings that are
    reapplied when the program exits
    */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode(void) {
    // Read current terminal attributes into struct orig_termios
    tcgetattr(STDIN_FILENO, &orig_termios);
    
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
        
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main(void) {
    enableRawMode();
    
    char c;
    while (1) {
        char c = '\0';
        read(STDIN_FILENO, &c, 1);
        if (iscntrl(c)) {
            // Prints ascii codes of control characters
            printf("%d\n", c);
        } else {
            // Prints ascii codes and character value
            printf("%d ('%c')\r\n", c, c);
        }
        
        if (c == 'q') break;
    }
    return 0;
}
