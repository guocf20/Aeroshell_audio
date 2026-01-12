# 设置编译器
CC = gcc
CXX = g++
AR = ar

# 编译选项
CFLAGS = -O2 -fPIC -I. -I./include
CXXFLAGS = -O2 -fPIC -I. -I./include -std=c++11

# 目标静态库名称
LIB_NAME = libwebrtc_vad.a

# 源码文件
C_SOURCES = webrtc_vad.c vad_core.c vad_filterbank.c vad_gmm.c vad_sp.c spl_inl.c
OBJS = $(C_SOURCES:.c=.o)

# 默认规则：生成静态库
all: $(LIB_NAME)

$(LIB_NAME): $(OBJS)
	$(AR) rcs $@ $(OBJS)
	@echo "Static library $(LIB_NAME) created."

# 编译 C 文件
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 清理
clean:
	rm -f *.o $(LIB_NAME)