CC = arm-none-eabi-gcc
AS = arm-none-eabi-as
OBJCOPY = arm-none-eabi-objcopy
GBAFIX = gbafix

CFLAGS = -mthumb -mthumb-interwork -mcpu=arm7tdmi -O2 -Wall -fomit-frame-pointer -ffast-math
LDFLAGS = -nostartfiles -T gba.ld -Wl,-Map=flxfsm.map

TARGET = flxfsm

all: $(TARGET).gba

$(TARGET).gba: $(TARGET).elf
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin
	$(GBAFIX) $(TARGET).bin -tFLXFSM -cFSM1 -m01 -r1
	mv $(TARGET).bin $(TARGET).gba

$(TARGET).elf: crt0.o main.o
	$(CC) crt0.o main.o $(LDFLAGS) -o $(TARGET).elf

crt0.o: crt0.s
	$(AS) -mcpu=arm7tdmi crt0.s -o crt0.o

main.o: main.c gba.h assets.h
	$(CC) $(CFLAGS) -c main.c -o main.o

clean:
	rm -f *.o *.elf *.bin *.gba *.map
