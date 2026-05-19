#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

/* Session management */
#define SESSION_DIR "/tmp/boa_sessions"
#define SESSION_VALID_SECS 3600
#define TOKEN_LEN 64

void cgi_header(const char *content_type);
void cgi_redirect(const char *url);
void cgi_error(const char *msg);
char *get_env(const char *key);
char *get_cookie(const char *name);
int session_create(const char *username, char *token_out);
int session_check(const char *token);
void session_destroy(const char *token);

/* Serial port */
#define SERIAL_DEVICE "/dev/ttyS3"
#define CUSTOM_BAUD 150000

int serial_open(const char *device, unsigned int baudrate);
int serial_send(int fd, const char *data, int len);
void serial_close(int fd);

/* CGI parameter parsing */
char *get_post_param(const char *param_name);

#endif /* COMMON_H */
