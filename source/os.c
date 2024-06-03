//GEMS BY SPARKSAMMY/NodeMixaholic
#include <string.h>
#include "strcmp.h" //Compare string lib
#include <string.h>
#include <unistd.h> //needed for time.h
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "time.h" //100000000 TPS time.
#include "standard_io.h" //I think this might have an issue. Not sure. (contains print and clear)
#include "pcspeaker.h" //PC speaker support
#include "debug.h" //panic and stuff
#include "midi.h" //PC speaker MIDI support
#include "kbd.h" //keyboard stuff
#include "thirdparty.h"
//#include "crashhand.h" //Comming Soon(TM)
//#include "sticky.h" //module deprecated. use new stick function and new function called wait.
//#include "diamondfs.h" //BORKED - USE AT YOUR OWN RISK
#include "rushell.h" // Should always be at the bottom (Testing shell, only displays output and plays "music." Does not actually function as a shell.)

#if defined(__linux__)
	#error "This code must be compiled with an x86-elf cross-compiler"
#elif !defined(__i386__)
	#error "This code must be compiled with an x86-elf cross-compiler"
#endif
typedef struct mboot_memmap {
	unsigned int size;
	unsigned int base_addr_low,base_addr_high;
	unsigned int type;
} mboot_memmap_t;

typedef struct multiboot_info {
	unsigned int mmap_addr;
	unsigned int mmap_length;
} multiboot_info;

typedef mboot_memmap_t mmap_entry_t;
typedef multiboot_info mmap_addr_length;

void exit() {
    panic("MISC CRASH");
}

void os() {
	rloadstring("welcomescreen", "", "");
	rloadstring("helpscreen", "", "");
	//panic("TEST"); //uncomment to test panicing
	rloadstring("debugRainbow", "", "");
	while ( 1 == 1 ) {
		rloadstring("basickeys", "", "");
	}
}

void kern() {
	//extern bootloader();
	//bootloader();
	clear(lastVGATextColor());
	print("GEMS OK, WAITING A FEW TICKS TO TEST TIME... \n");
	wait(10); //ok? ok.
	beep();
	print("Getting frequency of A2 \n");
	print("(might be garbled until fixed) \n");
	print(freqOfAsStr("a#2")); //wow.
	print("\n Testing int2str functionality... \n");
	printint(777);
	print(" Winner!");
	print("\n");
	print("Everything seems fine to me. Starting OS...");
	waitSecs(5);
	clear();
	os();
}

int kernel_main(struct multiboot_info* mbd, unsigned int magic) {
	mmap_entry_t* entry = mbd->mmap_addr;
	while(entry < mbd->mmap_addr + mbd->mmap_length) {
		entry = (mmap_entry_t*) ((unsigned int) entry + entry->size + sizeof(entry->size));
	}
	kern();
}
//and we all *shut* down...
//#include "keymap.h" // not ready yet...
