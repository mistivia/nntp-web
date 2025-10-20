#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

// Networking
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

// Libevent for HTTP Server
#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>

// Jansson for JSON parsing
#include <jansson.h>

#define RESPONSE_BUFFER_SIZE 256
#define NNTP_BUFFER_SIZE 1024

// 结构体：存储从HTTP请求解析出的帖子数据
struct nntp_post_data {
    const char *from;
    const char *newsgroups;
    const char *subject;
    const char *body;
};

// 结构体：存储NNTP服务器的配置信息
struct nntp_config {
    const char *host;
    int port;
};

// 函数声明
int post_to_nntp(const struct nntp_config *config, const struct nntp_post_data *post, const char *reply_to_msgid);
void http_request_handler(struct evhttp_request *req, void *arg);

// 从socket读取一行NNTP响应
int read_nntp_line(int sock, char *buffer, size_t size) {
    size_t i = 0;
    while (i < size - 1) {
        int n = read(sock, &buffer[i], 1);
        if (n <= 0) return -1; // 连接错误或关闭
        if (buffer[i] == '\n' && i > 0 && buffer[i-1] == '\r') {
            buffer[i-1] = '\0'; // 替换 \r\n 为 \0
            return 0;
        }
        i++;
    }
    return -1; // 缓冲区溢出
}

// 向NNTP服务器发送命令
int send_nntp_command(int sock, const char *command) {
    return write(sock, command, strlen(command));
}

/**
 * @brief 核心功能：连接NNTP服务器并发帖
 * @param config NNTP服务器配置
 * @param post 帖子内容
 * @param reply_to_msgid 如果是回复，则为被回复帖子的Message-ID，否则为NULL
 * @return 0表示成功，-1表示失败
 */
int post_to_nntp(const struct nntp_config *config, const struct nntp_post_data *post, const char *reply_to_msgid) {
    int sock = -1;
    struct sockaddr_in server_addr;
    struct hostent *server;
    char buffer[NNTP_BUFFER_SIZE];
    int status_code;

    // 1. 创建Socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("ERROR opening socket");
        return -1;
    }

    server = gethostbyname(config->host);
    if (server == NULL) {
        fprintf(stderr, "ERROR, no such host\n");
        close(sock);
        return -1;
    }

    // 2. 连接到服务器
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(config->port);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("ERROR connecting");
        close(sock);
        return -1;
    }

    // 3. 读取欢迎信息
    if (read_nntp_line(sock, buffer, sizeof(buffer)) != 0) goto nntp_error;
    sscanf(buffer, "%d", &status_code);
    printf("NNTP Welcome: %s\n", buffer);
    if (status_code != 200 && status_code != 201) {
        fprintf(stderr, "NNTP server not ready: %s\n", buffer);
        goto nntp_error;
    }
    
    // 5. 发送POST命令
    send_nntp_command(sock, "POST\r\n");
    if (read_nntp_line(sock, buffer, sizeof(buffer)) != 0) goto nntp_error;
    sscanf(buffer, "%d", &status_code);
    printf("NNTP POST: %s\n", buffer);
    if (status_code != 340) {
        fprintf(stderr, "NNTP server cannot accept post: %s\n", buffer);
        goto nntp_error;
    }

    // 6. 构造并发送文章头和内容
    // 生成一个唯一的Message-ID
    char message_id[256];
    char hostname[128];
    gethostname(hostname, sizeof(hostname)-1);
    snprintf(message_id, sizeof(message_id), "<%ld.%d@%s>", time(NULL), rand(), hostname);

    // 发送头
    snprintf(buffer, sizeof(buffer), "From: %s\r\n", post->from);
    send_nntp_command(sock, buffer);
    snprintf(buffer, sizeof(buffer), "Newsgroups: %s\r\n", post->newsgroups);
    send_nntp_command(sock, buffer);
    snprintf(buffer, sizeof(buffer), "Subject: %s\r\n", post->subject);
    send_nntp_command(sock, buffer);
    snprintf(buffer, sizeof(buffer), "Message-ID: %s\r\n", message_id);
    send_nntp_command(sock, buffer);
    snprintf(buffer, sizeof(buffer), "Content-Type: text/plain; charset=UTF-8\r\n");
    send_nntp_command(sock, buffer);
    snprintf(buffer, sizeof(buffer), "Content-Transfer-Encoding: 8bit\r\n");
    send_nntp_command(sock, buffer);

    // 如果是回复，添加References头
    if (reply_to_msgid) {
        snprintf(buffer, sizeof(buffer), "References: %s\r\n", reply_to_msgid);
        send_nntp_command(sock, buffer);
    }
    send_nntp_command(sock, "\r\n"); // 空行分隔头和正文

    // 7. 发送正文 (注意dot-stuffing)
    const char *line_start = post->body;
    const char *line_end;
    while ((line_end = strchr(line_start, '\n')) != NULL) {
        if (line_start[0] == '.') {
            send_nntp_command(sock, ".");
        }
        write(sock, line_start, line_end - line_start + 1);
        send_nntp_command(sock, "\r"); // HTTP body中是\n, NNTP需要\r\n
        line_start = line_end + 1;
    }
    // 处理最后一行
    if (*line_start) {
        if (line_start[0] == '.') {
            send_nntp_command(sock, ".");
        }
        send_nntp_command(sock, line_start);
        send_nntp_command(sock, "\r\n");
    }

    // 8. 发送结束符
    send_nntp_command(sock, ".\r\n");
    if (read_nntp_line(sock, buffer, sizeof(buffer)) != 0) goto nntp_error;
    sscanf(buffer, "%d", &status_code);
    printf("NNTP Result: %s\n", buffer);
    if (status_code != 240) {
        fprintf(stderr, "NNTP article post failed: %s\n", buffer);
        goto nntp_error;
    }
    
    // 9. 退出
    send_nntp_command(sock, "QUIT\r\n");
    read_nntp_line(sock, buffer, sizeof(buffer)); // 读取退出信息
    close(sock);
    return 0;

nntp_error:
    if (sock >= 0) {
        send_nntp_command(sock, "QUIT\r\n");
        close(sock);
    }
    return -1;
}

/**
 * @brief Libevent的HTTP请求回调函数
 */
void http_request_handler(struct evhttp_request *req, void *arg) {
    struct nntp_config *config = (struct nntp_config *)arg;
    
    // 只接受POST请求
    if (evhttp_request_get_command(req) == EVHTTP_REQ_OPTIONS) {
        struct evbuffer *buf = evbuffer_new();
        struct evkeyvalq *output_headers = evhttp_request_get_output_headers(req);
        evhttp_add_header(output_headers, "Access-Control-Allow-Origin", "*");
        evhttp_add_header(output_headers, "Access-Control-Allow-Methods", "POST");
        evhttp_add_header(output_headers, "Access-Control-Allow-Headers", "Content-Type, Authorization");
        evhttp_send_reply(req, HTTP_OK, "OK", buf);
        evbuffer_free(buf);
        return;
    } else if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        evhttp_send_error(req, HTTP_BADMETHOD, "Method Not Allowed");
        return;
    }

    struct evbuffer *buf = evhttp_request_get_input_buffer(req);
    size_t len = evbuffer_get_length(buf);
    char *post_body = malloc(len + 1);
    if (!post_body) {
        evhttp_send_error(req, HTTP_INTERNAL, "Internal Server Error");
        return;
    }
    evbuffer_remove(buf, post_body, len);
    post_body[len] = '\0';
    
    // 3. 解析JSON Body
    json_error_t error;
    json_t *root = json_loads(post_body, 0, &error);
    free(post_body);

    if (!root) {
        fprintf(stderr, "JSON error on line %d: %s\n", error.line, error.text);
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "Invalid JSON: %s", error.text);
        evhttp_send_error(req, HTTP_BADREQUEST, err_msg);
        return;
    }

    // 提取JSON字段
    struct nntp_post_data post_data;
    json_t *j_from = json_object_get(root, "from");
    json_t *j_newsgroups = json_object_get(root, "newsgroups");
    json_t *j_subject = json_object_get(root, "subject");
    json_t *j_body = json_object_get(root, "body");
    json_t *j_reply_to = json_object_get(root, "reply_to");

    if (!json_is_string(j_from) || !json_is_string(j_newsgroups) ||
        !json_is_string(j_subject) || !json_is_string(j_body)) {
        evhttp_send_error(req, HTTP_BADREQUEST, "Missing or invalid JSON fields: from, newsgroups, subject, body must be strings.");
        json_decref(root);
        return;
    }

    post_data.from = json_string_value(j_from);
    post_data.newsgroups = json_string_value(j_newsgroups);
    post_data.subject = json_string_value(j_subject);
    post_data.body = json_string_value(j_body);
    const char *reply_to = NULL;
    if (json_is_string(j_reply_to)) {
        reply_to = json_string_value(j_reply_to);
    }

    // 4. 调用NNTP发帖函数
    int result = post_to_nntp(config, &post_data, reply_to);

    // 5. 构造并发送HTTP响应
    struct evbuffer *resp_buf = evbuffer_new();
    if (result == 0) {
        if (reply_to) {
            printf("Successfully replied to %s\n", reply_to);
            evbuffer_add_printf(resp_buf, "{\"status\": \"success\", \"action\": \"reply\", \"reply_to\": \"%s\"}", reply_to);
        } else {
            printf("Successfully posted new article\n");
            evbuffer_add_printf(resp_buf, "{\"status\": \"success\", \"action\": \"new_post\"}");
        }
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Origin", "*");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Methods", "POST");
        evhttp_send_reply(req, HTTP_OK, "OK", resp_buf);
    } else {
        printf("Failed to post to NNTP server\n");
        evbuffer_add_printf(resp_buf, "{\"status\": \"error\", \"message\": \"Failed to post to NNTP server\"}");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Origin", "*");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Methods", "POST");
        evhttp_send_reply(req, HTTP_INTERNAL, "Bad Gateway", resp_buf);
    }

    // 6. 清理
    json_decref(root);
    evbuffer_free(resp_buf);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <web_listen_ip> <web_listen_port> <nntp_host> <nntp_port>\n", argv[0]);
        return 1;
    }

    const char *web_ip = argv[1];
    int web_port = atoi(argv[2]);
    struct nntp_config config;
    config.host = argv[3];
    config.port = atoi(argv[4]);

    // 忽略SIGPIPE信号，避免因客户端断开连接而导致进程退出
    signal(SIGPIPE, SIG_IGN);

    struct event_base *base = event_base_new();
    if (!base) {
        fprintf(stderr, "Could not initialize libevent!\n");
        return 1;
    }

    struct evhttp *http = evhttp_new(base);
    if (!http) {
        fprintf(stderr, "Could not create evhttp. Exiting.\n");
        return 1;
    }
    evhttp_set_allowed_methods(http,
	    EVHTTP_REQ_GET |
	    EVHTTP_REQ_POST |
	    EVHTTP_REQ_HEAD |
	    EVHTTP_REQ_PUT |
        EVHTTP_REQ_OPTIONS |
	    EVHTTP_REQ_DELETE);

    // 设置通用回调函数，将nntp_config传递给它
    evhttp_set_gencb(http, http_request_handler, &config);

    struct evhttp_bound_socket *handle = evhttp_bind_socket_with_handle(http, web_ip, web_port);
    if (!handle) {
        fprintf(stderr, "Could not bind to %s:%d. Exiting.\n", web_ip, web_port);
        return 1;
    }

    printf("NNTP Web Poster listening on %s:%d\n", web_ip, web_port);
    event_base_dispatch(base);

    // 清理
    evhttp_free(http);
    event_base_free(base);

    return 0;
}