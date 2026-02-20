CFLAGS = -Wall

OBJECTS = lab2.o fbputchar.o usbkeyboard.o key.o chat_utils.o

TARFILES = Makefile lab2.c \
	fbputchar.h fbputchar.c \
	usbkeyboard.h usbkeyboard.c \
	key.h key.c \
	chat_utils.h chat_utils.c

lab2 : $(OBJECTS)
	cc $(CFLAGS) -o lab2 $(OBJECTS) -lusb-1.0 -pthread

lab2.tar.gz : $(TARFILES)
	rm -rf lab2
	mkdir lab2
	ln $(TARFILES) lab2
	tar zcf lab2.tar.gz lab2
	rm -rf lab2

lab2.o : lab2.c fbputchar.h usbkeyboard.h key.h chat_utils.h
fbputchar.o : fbputchar.c fbputchar.h
usbkeyboard.o : usbkeyboard.c usbkeyboard.h
key.o : key.c key.h fbputchar.h
chat_utils.o : chat_utils.c chat_utils.h

.PHONY : clean
clean :
	rm -rf *.o lab2
