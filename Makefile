# target source
TARGET = rtsp_server
SRC = $(wildcard *.c)
OBJ = $(patsubst %.c, %.o, $(SRC))
CFLAGS = -O2

.PHONY : clean all

all: $(TARGET)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -lpthread -lm -o $@ $^

clean:
	@rm -f $(TARGET)
	@rm -f $(OBJ)
