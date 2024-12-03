#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "./cache/cache.h"
#include "./load_balancer/load_balancer.h"
#include "./health_check/health_check.h"

#define MAX_BUFFER_SIZE 655036
#define MAX_EVENTS 1000

// 요청을 처리하는 함수 프로토타입
void handle_request(int client_sock);
void send_response(int client_sock, const char *response_header, const char *response_body, int is_head, int response_size);

httpserver servers[] = {
    {"10.198.138.212", 12345, 3, 1},
    {"10.198.138.213", 12345, 10, 1}};

int server_count = 2;

// 전역 변수로 설정 값 선언
int PROXY_PORT;
char TARGET_SERVER1[256];
char TARGET_SERVER2[256];
int TARGET_PORT;
int CACHE_ENABLED;
int LOAD_BALANCER_MODE;

// 설정 파일에서 값을 읽어오는 함수
void load_config(const char *config_file)
{
    FILE *file = fopen(config_file, "r");
    if (!file)
    {
        perror("Failed to open config file");
        exit(EXIT_FAILURE);
    }

    char line[256];
    while (fgets(line, sizeof(line), file))
    {
        char key[256], value[256];
        if (sscanf(line, "%255[^=]=%255[^\n]", key, value) == 2)
        {
            if (strcmp(key, "PROXY_PORT") == 0)
            {
                PROXY_PORT = atoi(value);
            }
            else if (strcmp(key, "TARGET_SERVER1") == 0)
            {
                strncpy(TARGET_SERVER1, value, sizeof(TARGET_SERVER1) - 1);
                TARGET_SERVER1[sizeof(TARGET_SERVER1) - 1] = '\0';
            }
            else if (strcmp(key, "TARGET_SERVER2") == 0)
            {
                strncpy(TARGET_SERVER2, value, sizeof(TARGET_SERVER2) - 1);
                TARGET_SERVER2[sizeof(TARGET_SERVER2) - 1] = '\0';
            }
            else if (strcmp(key, "TARGET_PORT") == 0)
            {
                TARGET_PORT = atoi(value);
            }
            else if (strcmp(key, "CACHE_ENABLED") == 0)
            {
                CACHE_ENABLED = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) ? 1 : 0;
            }
            else if (strcmp(key, "LOAD_BALANCER_MODE") == 0)
            {
                LOAD_BALANCER_MODE = atoi(value);
            }
        }
    }
    fclose(file);
}

int main()
{
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // 설정 파일 읽기
    load_config("reverse_proxy.conf");

    init_http_servers(servers, 2);
    printf("Selected ip and port : %s, %d\n", servers[0].ip, servers[0].port);

    pthread_t health_thread;
    health_check_args args = {servers, server_count};
    if (pthread_create(&health_thread, NULL, health_check, &args) != 0)
    {
        perror("Failed to create health check thread");
        exit(EXIT_FAILURE);
    }
    pthread_detach(health_thread);

    // 캐시 초기화
    if (CACHE_ENABLED)
    {
        cache_init();
    }

    // 서버 소켓 생성
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // 포트 재사용 설정
    int optvalue = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optvalue, sizeof(optvalue)) < 0)
    {
        perror("setsockopt failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PROXY_PORT);

    // 서버 바인딩
    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // 서버 리슨
    if (listen(server_sock, 5) < 0)
    {
        perror("Listen failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", PROXY_PORT);

    while (1)
    {
        // 새로운 클라이언트 연결 수락
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock == -1)
        {
            perror("Accept failed");
            continue;
        }

        // 동기 방식으로 요청 처리
        handle_request(client_sock);
        close(client_sock); // 요청 처리 후 클라이언트 소켓 종료
    }

    close(server_sock);
    return 0;
}

void handle_request(int client_sock)
{
    int server_sock;
    struct sockaddr_in target_addr;
    char buffer[MAX_BUFFER_SIZE] = {0};
    char method[10] = {0};
    char url[256] = {0};
    char protocol[10] = {0};

    ssize_t bytes_read = read(client_sock, buffer, sizeof(buffer));
    if (bytes_read <= 0)
    {
        perror("Failed to read client request");
        close(client_sock);
        return;
    }

    // GET, HEAD 메서드 및 URL, 프로토콜 추출
    if (sscanf(buffer, "%s %s %s", method, url, protocol) != 3)
    {
        fprintf(stderr, "Failed to parse the request line properly\n");
        close(client_sock);
        return;
    }

    httpserver server = weighted_round_robin(); // 로드 밸런서 호출
    printf("Forwarding to server: %s:%d\n", server.ip, server.port);

    char cached_data[MAX_BUFFER_SIZE] = {0};
    int header_size = 0;
    char header_buffer[MAX_BUFFER_SIZE] = {0};
    int newline_count = 0;
    int max_newline = 4;

    if (CACHE_ENABLED && cache_lookup(url, cached_data))
    {
        // 캐시 히트 메시지 출력
        printf("Cache hit for URL: %s\n\n\n", url);

        if (strstr(cached_data, "Transfer-Encoding: chunked") != NULL)
        {
            max_newline = 5; // chunked가 있으면 \n 5개까지 찾기
            printf("this is chuncked\n");
        }

        for (int i = 0; i < sizeof(cached_data); i++)
        {
            header_size++;
            if (cached_data[i] == '\n')
            {
                newline_count++;
                if (newline_count == max_newline)
                {
                    break;
                }
            }
        }
        // 헤더와 본문을 분리
        memcpy(header_buffer, cached_data, header_size);

        send_response(client_sock, header_buffer, cached_data, strcmp(method, "HEAD") == 0, strlen(cached_data));
    }
    else
    {
        // 캐시 미스 처리
        printf("Cache miss for URL: %s\n\n\n", url);

        // 백엔드 서버와 연결
        server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock < 0)
        {
            perror("Socket creation failed");
            close(client_sock);
            return;
        }

        memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(server.port);
        inet_pton(AF_INET, server.ip, &target_addr.sin_addr);

        if (connect(server_sock, (struct sockaddr *)&target_addr, sizeof(target_addr)) < 0)
        {
            perror("Connection failed");
            close(client_sock);
            close(server_sock);
            return;
        }
        // 요청 전달
        if (write(server_sock, buffer, bytes_read) < 0)
        {
            perror("Failed to forward request");
            close(client_sock);
            close(server_sock);
            return;
        }
        // 응답 수신 및 스트리밍 방식으로 클라이언트로 전달
        ssize_t bytes_received;
        char response_buffer[MAX_BUFFER_SIZE] = {0};
        int response_size = 0;
        if (strstr(url, "/jpg") != NULL)
        {
            size_t chunk_size;
            while (1)
            {
                // 청크 헤더 읽기 (청크 크기)
                bytes_received = read(server_sock, buffer, MAX_BUFFER_SIZE);
                if (bytes_received <= 0)
                {
                    if (bytes_received == 0)
                    {
                        printf("Connection closed by server.\n");
                    }
                    else
                    {
                        perror("Read failed");
                    }
                    break;
                }
                buffer[bytes_received] = '\0';

                // 청크 크기 파싱
                sscanf(buffer, "%zx", &chunk_size);
                if (chunk_size == 0)
                {
                    printf("End of chunked transfer.\n");
                    break;
                }

                // 청크 데이터 읽기
                char *data_start = strstr(buffer, "\r\n") + 2; // 청크 헤더 이후 데이터 시작
                fwrite(data_start, 1, chunk_size, stdout);

                // 다음 청크로 이동
            }
        }
        else
        {
            while ((bytes_received = read(server_sock, response_buffer + response_size, sizeof(response_buffer) - response_size)) > 0)
            {
                response_size += bytes_received;
            }

            if (strstr(response_buffer, "Transfer-Encoding: chunked") != NULL)
            {
                max_newline = 5; // chunked가 있으면 \n 5개까지 찾기
                printf("this is chuncked\n");
            }

            for (int i = 0; i < sizeof(response_buffer); i++)
            {
                header_size++;
                if (response_buffer[i] == '\n')
                {
                    newline_count++;
                    if (newline_count == max_newline)
                    {
                        break;
                    }
                }
            }

            // 헤더와 본문을 분리
            memcpy(header_buffer, response_buffer, header_size);

            if (bytes_received < 0)
            {
                perror("Failed to receive response from backend server");
            }
            else
            {
                // 캐시에 응답 저장 (응답 크기 검사)
                if (CACHE_ENABLED)
                {
                    printf("Store response for URL: %s into cache\n\n\n", url);
                    cache_store(url, response_buffer);
                }
                // 클라이언트로 응답 전송
                send_response(client_sock, header_buffer, response_buffer + header_size, strcmp(method, "HEAD") == 0, response_size - header_size);
            }
        }
        close(server_sock);
    }
    close(client_sock);
    return NULL;
}

// HTTP 응답을 클라이언트로 전송하는 함수
void send_response(int client_sock, const char *response_header, const char *response_body, int is_head, int response_size)
{
    // 응답 헤더의 크기를 계산하여 Content-Length에 설정
    char header[512];
    snprintf(header, sizeof(header), response_header, response_size);

    // 헤더 전송
    if (write(client_sock, header, strlen(header)) < 0)
    {
        perror("Failed to send header");
        return;
    }

    // 본문이 있으면 본문 전송
    if (!is_head)
    {
        if (write(client_sock, response_body, response_size) < 0)
        {
            perror("Failed to send response body");
        }
    }
}