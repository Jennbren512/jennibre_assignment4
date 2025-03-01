#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>

#define INPUTLENGTH 2048

int allowBackground = 1;

void getInput(char*[], int*, char[], char[], int);
void excmd(char*[], int*, struct sigaction, int*, char[], char[]);
void catchSIGTSTP(int);
void printExitStatus(int);
void catchSIGCHLD(int);

/* Handle SIGTSTP (^Z) to toggle foreground-only mode */
void catchSIGTSTP(int signo) {
    if (allowBackground) {
        char* message = "Entering foreground-only mode (& is now ignored)\n";
        write(STDOUT_FILENO, message, strlen(message));
        allowBackground = 0;
    } else {
        char* message = "Exiting foreground-only mode\n";
        write(STDOUT_FILENO, message, strlen(message));
        allowBackground = 1;
    }
}

/* Handle SIGCHLD to detect when background processes exit */
void catchSIGCHLD(int signo) {
    int childExitStatus;
    pid_t childPid;

    while ((childPid = waitpid(-1, &childExitStatus, WNOHANG)) > 0) {
        printf("child %d terminated\n", childPid);
        printExitStatus(childExitStatus);
        fflush(stdout);
    }
}

/* Main function */
int main() {
    int pid = getpid();
    int cont = 1;
    int i;
    int exitStatus = 0;
    int background = 0;

    char inputFile[256] = "";
    char outputFile[256] = "";
    char* input[512] = {NULL};

    /* Ignore SIGINT (^C) */
    struct sigaction sa_sigint = {0};
    sa_sigint.sa_handler = SIG_IGN;
    sigfillset(&sa_sigint.sa_mask);
    sa_sigint.sa_flags = 0;
    sigaction(SIGINT, &sa_sigint, NULL);

    /* Handle SIGTSTP (^Z) */
    struct sigaction sa_sigtstp = {0};
    sa_sigtstp.sa_handler = catchSIGTSTP;
    sigfillset(&sa_sigtstp.sa_mask);
    sa_sigtstp.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa_sigtstp, NULL);

    /* Handle SIGCHLD (background process termination) */
    struct sigaction sa_sigchld = {0};
    sa_sigchld.sa_handler = catchSIGCHLD;
    sigfillset(&sa_sigchld.sa_mask);
    sa_sigchld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_sigchld, NULL);

    do {
        printf(": ");
        fflush(stdout);

        getInput(input, &background, inputFile, outputFile, pid);

        if (input[0] == NULL || input[0][0] == '#' || input[0][0] == '\0') {
            continue;
        } else if (strcmp(input[0], "exit") == 0) {
            cont = 0;
        } else if (strcmp(input[0], "cd") == 0) {
            if (input[1]) {
                if (chdir(input[1]) == -1) {
                    perror("cd");
                }
            } else {
                chdir(getenv("HOME"));
            }
        } else if (strcmp(input[0], "status") == 0) {
            printExitStatus(exitStatus);
        } else {
            excmd(input, &exitStatus, sa_sigint, &background, inputFile, outputFile);
        }

        for (i = 0; input[i]; i++) {
            free(input[i]);
            input[i] = NULL;
        }
        background = 0;
        inputFile[0] = '\0';
        outputFile[0] = '\0';

    } while (cont);

    return 0;
}

/* Prompts the user for input and parses it into an array */
void getInput(char* arr[], int* background, char inputName[], char outputName[], int pid) {
    char input[INPUTLENGTH];

    if (!fgets(input, INPUTLENGTH, stdin)) {
        clearerr(stdin);
        return;
    }

    input[strcspn(input, "\n")] = '\0';

    if (strlen(input) == 0) {
        arr[0] = strdup("");
        return;
    }

    const char space[2] = " ";
    char* token = strtok(input, space);
    int i = 0;

    while (token) {
        if (strcmp(token, "&") == 0) {
            *background = 1;
        } else if (strcmp(token, "<") == 0) {
            token = strtok(NULL, space);
            if (token) strcpy(inputName, token);
        } else if (strcmp(token, ">") == 0) {
            token = strtok(NULL, space);
            if (token) strcpy(outputName, token);
        } else {
            arr[i] = malloc(strlen(token) + 10);
            strcpy(arr[i], token);

            // Replace $$ with PID
            char* pidPos = strstr(arr[i], "$$");
            if (pidPos) {
                snprintf(arr[i], 256, "%.*s%d", (int)(pidPos - arr[i]), arr[i], pid);
            }
            i++;
        }
        token = strtok(NULL, space);
    }
    arr[i] = NULL;
}

/* Execute the command parsed into the array */
void excmd(char* arr[], int* childExitStatus, struct sigaction sa, int* background, char inputName[], char outputName[]) {
    int input, output;
    pid_t spawnPid = fork();

    switch (spawnPid) {
        case -1:
            perror("fork");
            exit(1);
            break;

        case 0: /* Child process */
            sa.sa_handler = SIG_DFL;
            sigaction(SIGINT, &sa, NULL);

            if (strcmp(inputName, "")) {
                input = open(inputName, O_RDONLY);
                if (input == -1) {
                    perror("open input");
                    exit(1);
                }
                dup2(input, 0);
                close(input);
            }

            if (strcmp(outputName, "")) {
                output = open(outputName, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (output == -1) {
                    perror("open output");
                    exit(1);
                }
                dup2(output, 1);
                close(output);
            }

            if (execvp(arr[0], arr) == -1) {
                perror("execvp");
                exit(2);
            }
            break;

        default: /* Parent process */
            if (*background && allowBackground) {
                printf("background pid is %d\n", spawnPid);
                fflush(stdout);
                waitpid(spawnPid, childExitStatus, WNOHANG);
            } else {
                waitpid(spawnPid, childExitStatus, 0);
            }
    }
}

/* Print exit status */
void printExitStatus(int childExitMethod) {
    if (WIFEXITED(childExitMethod)) {
        printf("exit value %d\n", WEXITSTATUS(childExitMethod));
    } else {
        printf("terminated by signal %d\n", WTERMSIG(childExitMethod));
    }
    fflush(stdout);
}
