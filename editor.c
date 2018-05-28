/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <strings.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)
#define EDITOR_VERSION "0.0.1"

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY
};

/*** data ***/

typedef struct editorrow {
    int length;
    char *text;
} editorrow;

struct editorConfig {
    struct termios originalTermi;
    int screenrows; // 1 indexed
    int screencols; // 1 indexed
    int cursorX; // 0 indexed
    int cursorY; // 0 indexed
    editorrow* erow;
    int numrows; // 1 indexed
    int rowOff; // 0 indexed
    int colOff; // 0 indexed
} E;

/*** struct append buffer ***/
#define APPEND_BUFFER_INIT {NULL, 0}

struct AppendBuffer {
    char *buffer;
    int length;
};

void abAppend(struct AppendBuffer* ab, char* s, int len) {
    char* new = realloc(ab->buffer, ab->length + len);

    if (new == NULL) {
        return ;
    }
    memcpy(&new[ab->length], s, len);
    ab->buffer = new;
    ab->length += len;
}

void abFree(struct AppendBuffer *ab) {
    free(ab->buffer);
}

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.originalTermi) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.originalTermi) == -1) 
        die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.originalTermi;
    raw.c_iflag = raw.c_iflag & (~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    raw.c_oflag = raw.c_oflag & (~OPOST);
    raw.c_cflag = raw.c_cflag | (CS8);
    raw.c_lflag = raw.c_lflag & (~(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) 
    die("tcsetattr");
}

int editorReadKey() {
    int nread;
    char ch;

    while ((nread = read(STDIN_FILENO, &ch, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) 
            die("read");
    }

    if (ch == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';


        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
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
                switch(seq[1]) {
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
        return ch;
    }
    
}

int getCursorPosition(int *rows, int *cols) {
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    
    char buffer[32];
    unsigned int i = 0;
    while (i < sizeof(buffer)) {
        if (read(STDIN_FILENO, &buffer[i], 1) != 1) break;

        if (buffer[i] == 'R') break;
        i++;
    }
    buffer[i] = '\0';
    if (buffer[0] != '\x1b' || buffer[1] != '[') return -1;
    if (sscanf(&buffer[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int * row, int * col) {

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(row, col);
    } else {
        *row = ws.ws_row;
        *col = ws.ws_col;
    }

    return 0;
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len) {
    E.erow = realloc(E.erow, sizeof(editorrow) * (E.numrows + 1));

    int pos = E.numrows;
    E.erow[pos].length = len; // excluding '\0' at the end of string
    E.erow[pos].text = malloc(len + 1);
    memcpy(E.erow[pos].text, s, len+1);
    E.numrows++;
}

/*** file I/O ***/

void editorOpen(char * file) {
    FILE *fp = fopen(file, "r");
    if (!fp) {
        die("fopen");
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen-1] == '\n' || line[linelen - 1] == '\r')) {
            linelen--;
        }

        editorAppendRow(line, linelen);
    }

    free(line);
    fclose(fp);
}

/*** output ***/

void editorDrawRows(struct AppendBuffer* ab) {
    for (int i = 0 ; i < E.screenrows ; i++) {
        int fileRow = i + E.rowOff;
        if (fileRow >= E.numrows) {
            if (E.numrows == 0 && i == E.screenrows / 2) {
                char welcome[80];

                int welcomeLen = snprintf(welcome, sizeof(welcome), "Text Editor -- version %s", EDITOR_VERSION);
                if (welcomeLen > E.screencols) welcomeLen = E.screencols;
                
                int leftPadding = (E.screencols - welcomeLen) / 2;
                if (leftPadding) {
                    abAppend(ab, "~", 1);
                }
                while (leftPadding > 0) {
                    abAppend(ab, " ", 1);
                    leftPadding--;
                }

                abAppend(ab, welcome, welcomeLen);
            } else {
                abAppend(ab, "~", 1);
            }  
        } else {
            int len = E.erow[fileRow].length - E.colOff;

            if (len < 0) {
                len = 0; // when user goes past the current line
            }
            if (len > E.screencols) {
                len = E.screencols;
            }
            abAppend(ab, &E.erow[fileRow].text[E.colOff], len);
        }
        
        abAppend(ab, "\x1b[K", 3); // clear rest of current line
        if (i < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    } 
}

void editorScroll() {
    if (E.cursorY < E.rowOff) { // going past top of the screen
        E.rowOff = E.cursorY;
    }

    if (E.cursorY >= E.rowOff + E.screenrows) { // going past bottom of the screen
        E.rowOff = E.cursorY - E.screenrows + 1;
    }

    if (E.cursorX < E.colOff) { // going past left of the screen
        E.colOff = E.cursorX;
    }

    if (E.cursorX >= E.screencols + E.colOff) { // going past right of the screen
        E.colOff = E.cursorX - E.screencols + 1;
    }
}

void editorRefreshTerminal() {
    editorScroll();

    struct AppendBuffer ab = APPEND_BUFFER_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // hide cursor
    abAppend(&ab, "\x1b[H", 3); // bring cursor back up

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cursorY - E.rowOff + 1, E.cursorX - E.colOff + 1); // cursorX and cursorY are 0 indexed
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // show cursor

    write(STDOUT_FILENO, ab.buffer, ab.length);
    abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int c) {
    editorrow *erow = E.cursorY < E.numrows ? &E.erow[E.cursorY] : NULL;

    switch(c) {
        case ARROW_LEFT:
            if (E.cursorX != 0) {
                E.cursorX--;
            }
            break;
        case ARROW_DOWN:
            if (E.cursorY < E.numrows) { // allow scroll till one line past end of file
                E.cursorY++;
            }
            break;
        case ARROW_RIGHT:
            if (erow && E.cursorX < erow->length) { // allow scroll till one char past end of line
                E.cursorX++;
            }
            break;
        case ARROW_UP:
            if (E.cursorY != 0) {
                E.cursorY--;
            }
            break;
    }

    erow = E.cursorY < E.numrows ? &E.erow[E.cursorY] : NULL; // cursorY may be different, hence calculate again
    int len = erow ? erow->length : 0;
    if (E.cursorX > len) {
        E.cursorX = len;
    }
}

void editorProcessKey() {
    int c = editorReadKey();
    switch (c) {
        case CTRL_KEY('q') :         // exit on CTrl+Q
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case ARROW_LEFT:
        case ARROW_DOWN:
        case ARROW_RIGHT:
        case ARROW_UP:
            editorMoveCursor(c);
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int ii = E.screenrows;
                while (ii--) {
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;
        case HOME_KEY:
            E.cursorX = 0;
            break;
        case END_KEY:
            E.cursorX = E.screencols - 1;
            break;
    }
}

/*** init ***/

void initEditor() {
    E.cursorX = 0;
    E.cursorY = 0;
    E.numrows = 0;
    E.erow = NULL;
    E.rowOff = 0;
    E.colOff = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getWindowSize");
    }
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }
    
    while (1) {
        editorRefreshTerminal();
        editorProcessKey();
    }
    return 0;
}
