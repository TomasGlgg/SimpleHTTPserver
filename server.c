#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"


char INDEX[] = "index.html";
char FILE_NOT_FOUND_TEXT[] = "<h1>404 Not found</h1>";
char INTERNAL_SERVER_ERROR_TEXT[] = "<h1>500 Internal server error</h1>";
int MAX_LISTENER_QUEUE = 10;
int HEADERS_BUFFER = 4096;


int create_listener(int port) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket");
        exit(1);
    }

    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        exit(1);
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        exit(1);
    }

    listen(listener, MAX_LISTENER_QUEUE);
    return listener;
}


void recvline(int socket, char* buffer) {
    int received = 0;
    received += recv(socket, buffer, HEADERS_BUFFER - 1, 0);
    while (strchr(buffer, '\n') == NULL && received < HEADERS_BUFFER - 1) {
        received += recv(socket, buffer + received, HEADERS_BUFFER - received - 1, 0);
    }
    buffer[received] = '\0';
}

char* handle_headers(int socket) {
    char* file_path;
    char* headers[HEADERS_BUFFER];
    recvline(socket, headers);

    // parsing GET request
    char* file_path_pos_start = strstr(headers, "GET") + 5; // length of GET + space + '/'
    if (file_path_pos_start == NULL) return NULL;
    char* file_path_pos_end = strchr(file_path_pos_start, ' '); // end of file path (after GET)
    int file_path_length = file_path_pos_end - file_path_pos_start;
    if (file_path_length) {
        file_path = malloc(sizeof(char) * (file_path_length+1)); // + null byte
        strncpy(file_path, file_path_pos_start, file_path_length);
        file_path[file_path_length] = '\0'; // c str
    } else {
        file_path = malloc(sizeof(char) * (sizeof(INDEX))); // INDEX
        strncpy(file_path, INDEX, sizeof(INDEX));
    }
    if (file_path[0] == '/') return NULL; // '//' at start
    return file_path;
}

int check_file(char* file_path) {
    if (strstr(file_path, "..") != NULL) return -1; // simple protection against path poisoning

    struct stat st;
    if(stat(file_path, &st) < 0) return -1;

    switch (st.st_mode & S_IFMT) {
        case S_IFREG: // is file
            return st.st_size;

        case S_IFDIR: // is directory
            // checking directory/INDEX
            file_path = realloc(file_path, strlen(file_path) + 1 + strlen(INDEX));
            strcat(file_path, "/");
            strcat(file_path, INDEX);

            if (stat(file_path, &st) < 0) return -1;
            if (st.st_mode & S_IFMT != S_IFREG) return -1; // check is file
            return st.st_size;

        default:
            return -1;
    }
}

void send_headers(int socket, char* status_code, int file_size) {
    char headers[HEADERS_BUFFER];
    int len = sprintf(headers, "HTTP/1.1 %s\nContent-Length: %d\n\n", status_code, file_size);
    write(socket, headers, len);
}

void send_404(int socket) {
    send_headers(socket, "404 Not Found", sizeof(FILE_NOT_FOUND_TEXT));
    write(socket, FILE_NOT_FOUND_TEXT, sizeof(FILE_NOT_FOUND_TEXT));
}

void send_500(int socket) {
    send_headers(socket, "500 Internal server error", sizeof(INTERNAL_SERVER_ERROR_TEXT));
    write(socket, INTERNAL_SERVER_ERROR_TEXT, sizeof(INTERNAL_SERVER_ERROR_TEXT));
}

void handle_socket(int socket) {
    char* file_path = handle_headers(socket);
    if (file_path == NULL) return;
    int file_size;
    printf("File: %s - ", file_path);

    if ((file_size = check_file(file_path)) < 0) {
        printf("not found\n");
        send_404(socket);
        goto exit;
    }

    FILE* file = fopen(file_path, "r");
    if (!file) {
        printf("failed to open\n");
        send_500(socket);
        goto fclose_and_exit;
    }
    printf("OK, size: %d\n", file_size);
    send_headers(socket, "200 OK", file_size);
    if (sendfile(socket, fileno(file), 0, file_size) < 0) perror("sendfile");  // send file content

fclose_and_exit:
    fclose(file);

exit:
    free(file_path);
    shutdown(socket, SHUT_RDWR);
    close(socket);
}

void handle_listener(int listener) {
    while (1) {
        int socket = accept(listener, NULL, NULL);
        if (socket < 0) {
            perror("accept");
            exit(1);
        }
        printf("Connection accepted\n");

        // call handle_socket() in new thread
        switch(fork()) {
            case 0: { // child
                handle_socket(socket);
                break;
            }
            case -1: { // error
                perror("fork");
                exit(1);
            }
            // default: parent
        }

    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        printf("Usage: %s <WORKING DIR> <PORT>", argv[0]);
        return 1;
    }
    int port = atoi(argv[2]);
    chdir(argv[1]);

    int listener = create_listener(port);
    printf("Listener started on %d\n", port);
    handle_listener(listener);
}
