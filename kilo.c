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
    raw.c_lflag &= ~(ECHO | ICANON);
        
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main(void) {
    enableRawMode();
    
    char c;

    while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q');
    return 0;
}
