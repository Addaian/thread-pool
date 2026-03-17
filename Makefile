CXX       := clang++
CXXFLAGS  := -std=c++17 -Wall -Wextra -Wpedantic -pthread
RELEASE   := -O2 -DNDEBUG
DEBUG     := -g -fsanitize=thread -O1

SRC       := src/main.cpp
TARGET    := thread_pool
TARGET_DBG := thread_pool_dbg

.PHONY: all clean debug

all: $(TARGET)

$(TARGET): $(SRC) include/ThreadPool.h
	$(CXX) $(CXXFLAGS) $(RELEASE) -Iinclude -o $@ $(SRC)

debug: $(SRC) include/ThreadPool.h
	$(CXX) $(CXXFLAGS) $(DEBUG) -Iinclude -o $(TARGET_DBG) $(SRC)

clean:
	rm -f $(TARGET) $(TARGET_DBG)
