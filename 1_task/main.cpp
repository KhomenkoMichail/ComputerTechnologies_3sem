#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>

typedef struct input_t {
    char *inputBuf;
    size_t commandsNum;
    char ***tokens;
} input_t;

size_t getNumOfChar(const char *str, char searchedChar)
{
    assert(str);

    size_t numOfChar = 0;

    for (size_t i = 0; str[i] != '\0'; ++i) {
        if (str[i] == searchedChar)
            ++numOfChar;
    }

    return numOfChar;
}

size_t getNumOfTokens(const char *str)
{
    assert(str);

    size_t numOfTokens = 0;
    int isNewToken = 1;

    for (size_t i = 0; str[i] != '\0'; ++i) {
        if (isspace(unsigned char)(str[i])) {
            isNewToken = 1;
        } else if (isNewToken) {
            ++numOfTokens;
            isNewToken = 0;
        }
    }

    return numOfTokens;
}

void cleanup(input_t *input)
{
    if (!input)
        return;

    if (input->tokens != NULL) {
        for (size_t i = 0; input->tokens[i]; ++i)
            free(input->tokens[i]);

        free(input->tokens);
    }

    free(input->inputBuf);

    input->inputBuf = NULL;
    input->tokens = NULL;
}

char** getCommands (input_t *input) {
    assert(input);

    input->commandsNum = getNumOfChar(input->inputBuf, '|') + 1;
    char **commands = (char **)calloc(input->commandsNum + 1, sizeof(*commands));

    if (!commands) {
        perror("calloc");
        return NULL;
    }

    size_t commandsRead = 0;
    char *curCmd = strtok(input->inputBuf, "|");

    while (curCmd != NULL && commandsRead < input->commandsNum) {
        commands[commandsRead++] = curCmd;
        curCmd = strtok(NULL, "|");
    }

    if (commandsRead != input->commandsNum) {
        fprintf(stderr, "Error: empty command\n");
        free(commands);
        return NULL;
    }

    return commands;
}

int getTokens(input_t * input, char** commands) {
    assert(input);
    assert(commands);
    assert(*commands);

    input->tokens = (char ***)calloc(input->commandsNum + 1, sizeof(*input->tokens));
    if (!input->tokens) {
        perror("calloc");
        return -1;
    }

    const char tokensDelim[] = " \t\r\n";

    for (size_t curCmdIndex = 0; curCmdIndex < input->commandsNum; ++curCmdIndex) {
        size_t numOfTokens = getNumOfTokens(commands[curCmdIndex]);

        if (numOfTokens == 0) {
            fprintf(stderr, "Error: empty command\n");
            return -1;
        }

        input->tokens[curCmdIndex] = (char **)calloc(numOfTokens + 1, sizeof(*input->tokens[curCmdIndex]));
        if (!input->tokens[curCmdIndex]) {
            perror("calloc");
            return -1;
        }

        size_t tokNum = 0;
        char *curTok = strtok(commands[curCmdIndex], tokensDelim);

        while (curTok != NULL) {
            input->tokens[curCmdIndex][tokNum++] = curTok;
            curTok = strtok(NULL, tokensDelim);
        }

        input->tokens[curCmdIndex][tokNum] = NULL;
    }

    input->tokens[input->commandsNum] = NULL;

    return 0;
}


int getInput(input_t *input)
{
    assert(input);

    size_t inputSize = 0;
    ssize_t numOfReadSymbols = getline(&input->inputBuf, &inputSize, stdin);
    if (numOfReadSymbols == -1) {
        perror("getline");
        cleanup(input);
        return 0;
    }

    char **commands = getCommands(input);
    if (!commands) {
        cleanup(input);
        return -1;
    }

    if (getTokens(input, commands) == -1) {
        free(commands);
        cleanup(input);
        return -1;
    }

    free(commands);
    return 1;
}

void printProcessStatus(pid_t pid, const char *command, int status)
{
    if (WIFEXITED(status)) {
        fprintf(stderr, "[%d] %s: exit code %d\n", pid, command, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[%d] %s: signal %d\n", pid, command, WTERMSIG(status));
    }
}

void seqPipe(char ***cmd)
{
    int p[2] = {};
    pid_t pid = 0;
    int fd_in = STDIN_FILENO;
    int i = 0;

    while (cmd[i] != NULL) {
        if (pipe(p) == -1) {
            perror("pipe");

            if (i > 0)
                close(fd_in);

            return;
        }

        if ((pid = fork()) == -1) {
            perror("fork");

            close(p[0]);
            close(p[1]);

            if (i > 0)
                close(fd_in);

            return;
        }

        if (pid == 0) {
            if (i > 0) {
                if (dup2(fd_in, STDIN_FILENO) == -1) {
                    perror("dup2 stdin");
                    _exit(126);
                }
                close(fd_in);
            }

            if (cmd[i + 1] != NULL) {
                if (dup2(p[1], STDOUT_FILENO) == -1) {
                    perror("dup2 stdout");
                    _exit(126);
                }
            }

            close(p[0]);
            close(p[1]);

            execvp(cmd[i][0], cmd[i]);

            fprintf(stderr, "%s: %s\n", cmd[i][0],strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }

        int status = 0;
        pid_t waitResult = 0;

        do {
            waitResult = waitpid(pid, &status, 0);
        } while (waitResult == -1 && errno == EINTR);

        if (waitResult == -1) {
            perror("waitpid");

            close(p[0]);
            close(p[1]);

            if (i > 0) {
                close(fd_in);
            }

            return;
        }

        printProcessStatus(pid, cmd[i][0], status);

        close(p[1]);

        if (i > 0) {
            close(fd_in);
        }
        fd_in = p[0];

        ++i;
    }

    if (i > 0) {
        close(fd_in);
    }
}

int main(void)
{
    while (1) {

        input_t input = {};
        int result = getInput(&input);

        if (result == 0)
            break;

        if (result == -1)
            continue;

        if (input.tokens[0] != NULL && input.tokens[1] == NULL && strcmp(input.tokens[0][0], "exit") == 0) {
            cleanup(&input);
            break;
        }

        seqPipe(input.tokens);
        cleanup(&input);
    }

    return 0;
}
