# 编译器
CC = gcc

# 编译选项（添加 -g 以便调试，可选 -O2 优化）
CFLAGS = -Wall -Wextra -g

LDFLAGS = -laio -lpthread
# 头文件路径（如果有子目录可以加 -Iinclude 等）
INCLUDES = 

# 源文件
SRCS = src/libaio_run.c src/librio.c src/raidio.c

# 目标文件（.o）
OBJS = $(SRCS:.c=.o)

# 输出的程序名称
TARGET = rio

# 默认目标
all: $(TARGET)

# 链接
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LDFLAGS)

# 编译每个 .c 为 .o
%.o: %.c raidio.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# 清理目标
clean:
	rm -f $(OBJS) $(TARGET)

