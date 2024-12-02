#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include "cache.h"
#include "load_balancer.h"

#define MAX_BUFFER_SIZE 1024
#define MAX_EVENTS 10

// 요청을 처리하는 함수 프로토타입
void handle_request(int client_sock);
void send_response(int client_sock, const char *response, int is_head, int response_size);
httpserver select_server_based_on_mode();

// 전역 변수로 설정 값 선언
int PROXY_PORT;
char TARGET_SERVER1[256];
char TARGET_SERVER2[256];
int TARGET_PORT;
int CACHE_ENABLED;
int LOAD_BALANCER_MODE;
// 설정 파일에서 값을 읽어오는 함수
void load_config(const char *config_file) {
    FILE *file = fopen(config_file, "r");
    if (!file) {
        perror("Failed to open config file");
        exit(EXIT_FAILURE);
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char key[256], value[256];
        if (sscanf(line, "%255[^=]=%255[^\n]", key, value) == 2) {
            if (strcmp(key, "PROXY_PORT") == 0) {
                PROXY_PORT = atoi(value);
            } else if (strcmp(key, "TARGET_SERVER1") == 0) {
                strncpy(TARGET_SERVER1, value, sizeof(TARGET_SERVER1) - 1);
                TARGET_SERVER1[sizeof(TARGET_SERVER1) - 1] = '\0';
            } else if (strcmp(key, "TARGET_SERVER2") == 0) {
                strncpy(TARGET_SERVER2, value, sizeof(TARGET_SERVER2) - 1);
                TARGET_SERVER2[sizeof(TARGET_SERVER2) - 1] = '\0';
            } else if (strcmp(key, "TARGET_PORT") == 0) {
                TARGET_PORT = atoi(value);
            } else if (strcmp(key, "CACHE_ENABLED") == 0) {
                CACHE_ENABLED = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) ? 1 : 0;
            } else if (strcmp(key, "LOAD_BALANCER_MODE") == 0) {
                LOAD_BALANCER_MODE = atoi(value);
            }
        }
    }
    fclose(file);
}

int main() {
    int server_sock, client_sock, epoll_fd;
    struct sockaddr_in server_addr;
    struct epoll_event ev, events[MAX_EVENTS];
    socklen_t client_len;

    // 설정 파일 읽기
    load_config("reverse_proxy.conf");

    httpserver servers[] = {
        {"10.198.138.212", 12345, 3},
        {"10.198.138.213", 12345, 10}
    };
    init_http_servers(servers, 2);
    printf("Selected ip and port : %s, %d\n", servers[0].ip, servers[0].port);

    // 캐시 초기화
    if (CACHE_ENABLED) {
        cache_init();
    }

    // 서버 소켓 생성
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // 포트 재사용 설정
    int optvalue = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optvalue, sizeof(optvalue)) < 0) {
        perror("setsockopt failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PROXY_PORT);

    // 서버 바인딩
    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // 서버 리슨
    if (listen(server_sock, 5) < 0) {
        perror("Listen failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", PROXY_PORT);

    // epoll 생성
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // 서버 소켓을 epoll에 등록
    ev.events = EPOLLIN;
    ev.data.fd = server_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &ev) == -1) {
        perror("epoll_ctl failed");
        close(server_sock);
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }

    while (1) {
        // 이벤트 대기
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_sock) {
                // 새로운 클라이언트 연결 수락
                client_sock = accept(server_sock, NULL, NULL);
                if (client_sock == -1) {
                    perror("Accept failed");
                    continue;
                }

                // 동기 방식으로 요청 처리
                handle_request(client_sock);
            }
        }
    }

    close(server_sock);
    close(epoll_fd);
    return 0;
}


void handle_request(int client_sock) {
    int server_sock;
    struct sockaddr_in target_addr;
    char buffer[MAX_BUFFER_SIZE] = {0};
    char method[10] = {0};
    char url[256] = {0};
    char protocol[10] = {0};

    ssize_t bytes_read = read(client_sock, buffer, sizeof(buffer));
    if (bytes_read <= 0) {
        perror("Failed to read client request");
        close(client_sock);
        return;
    }

    // 디버깅: 수신된 요청 출력
    printf("Request content:\n%.*s\n", (int)bytes_read, buffer);

    // GET, HEAD 메서드 및 URL, 프로토콜 추출
    if (sscanf(buffer, "%s %s %s", method, url, protocol) != 3) {
        fprintf(stderr, "Failed to parse the request line properly\n");
        close(client_sock);
        return;
    }

    httpserver server = select_server_based_on_mode(); //로드 밸런서 호출
    printf("Selected server: %s:%d\n", server.ip, server.port);

    // 디버깅: 파싱된 결과 출력
    printf("Parsed method: %s, URL: %s, Protocol: %s\n", method, url, protocol);

    // URL 유효성 검사
    if ((strcmp(method, "GET") != 0) && (strcmp(method, "HEAD") != 0)) {
        fprintf(stderr, "Invalid request method: %s\n", method);
        close(client_sock);
        return;
    }

    char cached_data[MAX_BUFFER_SIZE] = {0};
    if (CACHE_ENABLED && cache_lookup(url, cached_data)) {
        // 캐시 히트 메시지 출력
        printf("Cache hit for URL: %s\n\n\n", url);
        send_response(client_sock, cached_data, strcmp(method, "HEAD") == 0, strlen(cached_data));  // HEAD일 경우 본문 제외
    } else {
        // 캐시 미스 처리
        printf("Cache miss for URL: %s\n\n\n", url);

        server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock < 0) {
            perror("Socket creation failed");
            close(client_sock);
            return;
        }

        memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(server.port);
        inet_pton(AF_INET, server.ip, &target_addr.sin_addr);

        if (connect(server_sock, (struct sockaddr *)&target_addr, sizeof(target_addr)) < 0) {
            perror("Connection failed");
            close(client_sock);
            close(server_sock);
            return;
        }

        // 요청 전달
        if (write(server_sock, buffer, bytes_read) < 0) {
            perror("Failed to forward request");
            close(client_sock);
            close(server_sock);
            return;
        }

        // 응답 수신 및 스트리밍 방식으로 클라이언트로 전달
        ssize_t bytes_received;
        char response_buffer[MAX_BUFFER_SIZE] = {0};
        int response_size = 0;
        while ((bytes_received = read(server_sock, response_buffer + response_size, sizeof(response_buffer) - response_size)) > 0) {
            response_size += bytes_received;
        }

        if (bytes_received < 0) {
            perror("Failed to receive response from backend server");
        } else {
            // 캐시에 응답 저장 (응답 크기 검사)
            if (CACHE_ENABLED) {
                printf("Store response for URL: %s into cache\n\n\n", url);
                cache_store(url, response_buffer);
            }

            // 클라이언트로 응답 전송
            send_response(client_sock, response_buffer, strcmp(method, "HEAD") == 0, response_size);
        }

        close(server_sock);
    }

    close(client_sock);
}


httpserver select_server_based_on_mode() {
    httpserver server;
    if (LOAD_BALANCER_MODE == 0) {
        server = round_robin();
        printf("Load Balancer Mode: Round Robin\n");
    } else if (LOAD_BALANCER_MODE == 1) {
        server = weighted_round_robin();
        printf("Load Balancer Mode: Weighted Round Robin\n");
    } else if (LOAD_BALANCER_MODE == 2) {
        server = least_connection();
        printf("Load Balancer Mode: Least Connection\n");
    } else {
        // 로드 밸런서를 사용하지 않는 경우 첫 번째 서버로 고정
        strcpy(server.ip, "10.198.138.212");
        server.port = 12345;
        printf("Load Balancer Mode: None (Fixed server selected)\n");
    }

    return server;
}



// HTTP 응답을 클라이언트로 전송하는 함수
void send_response(int client_sock, const char *response, int is_head, int response_size) {
    // HTTP/1.1 응답 헤더 작성 (Content-Type은 텍스트로 설정)
    const char *header_format = "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/plain\r\n"  // 응답의 타입 설정
                                "Connection: close\r\n"         // 연결 종료 헤더
                                "Content-Length: %d\r\n"        // 본문 길이
                                "\r\n";  // 헤더와 본문 구분

    // 응답 헤더의 크기를 계산하여 Content-Length에 설정
    char header[512];
    snprintf(header, sizeof(header), header_format, response_size);

    // 헤더 전송
    if (write(client_sock, header, strlen(header)) < 0) {
        perror("Failed to send header");
        return;
    }

    // 본문이 있으면 본문 전송
    if (!is_head) {
        if (write(client_sock, response, response_size) < 0) {
            perror("Failed to send response body");
        }
    }
}

