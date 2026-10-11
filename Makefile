CFLAGS ?= -O2 -Wall -Wextra

challenger: challenger.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f challenger
