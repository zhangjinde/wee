#ifndef BUFFER_H
#define BUFFER_H

#include <stdint.h>

struct buffer;

struct buffer *buf_create(size_t size);
void buf_release(struct buffer *buf);

size_t buf_readable(struct buffer *buf);
size_t buf_writable(struct buffer *buf);
size_t buf_prependable(struct buffer *buf);
const char *buf_peek(struct buffer *buf);
char *buf_beginWrite(struct buffer *buf);
void buf_has_written(struct buffer *buf, size_t len);
void buf_unwrite(struct buffer *buf, size_t len);
const char *buf_findCRLF(struct buffer *buf);
const char *buf_findEOL(struct buffer *buf);
void buf_retrieveAll(struct buffer *buf);
void buf_retrieve(struct buffer *buf, size_t len);
void buf_retrieveUntil(struct buffer *buf, const char *end);
void buf_retrieveInt64(struct buffer *buf);
void buf_retrieveInt32(struct buffer *buf);
void buf_retrieveInt16(struct buffer *buf);
void buf_retrieveInt8(struct buffer *buf);
void buf_append(struct buffer *buf, const char *data, size_t len);
void buf_prepend(struct buffer *buf, const char *data, size_t len);
void buf_shrink(struct buffer *buf, size_t reserve);
void buf_appendInt64(struct buffer *buf, int64_t x);
void buf_appendInt32(struct buffer *buf, int32_t x);
void buf_appendInt16(struct buffer *buf, int16_t x);
void buf_appendInt8(struct buffer *buf, int8_t x);
void buf_prependInt64(struct buffer *buf, int64_t x);
void buf_prependInt32(struct buffer *buf, int32_t x);
void buf_prependInt16(struct buffer *buf, int16_t x);
void buf_prependInt8(struct buffer *buf, int8_t x);
int64_t buf_peekInt64(struct buffer *buf);
int32_t buf_peekInt32(struct buffer *buf);
int16_t buf_peekInt16(struct buffer *buf);
int8_t buf_peekInt8(struct buffer *buf);
int64_t buf_readInt64(struct buffer *buf);
int32_t buf_readInt32(struct buffer *buf);
int16_t buf_readInt16(struct buffer *buf);
int8_t buf_readInt8(struct buffer *buf);
size_t buf_internalCapacity(struct buffer *buf);
ssize_t buf_readFd(struct buffer *buf, int fd, int *errno_);

#endif