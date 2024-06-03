//Made to create time at 100000000 TPS
//by Sparksammy
int time = 1;
int halted = 0;
void countOld() {
	static long long int i; 
	static int state = 0; 
		switch (state) 
		{ 
		case 0: /* start of function */
		while (1 == 1) {
			time++;
		}
		/* Returns control */
		case 1:; /* resume control straight 
		after the return */
		}
		state = 0; 
		return 0; 
	
}

void halt(int halt) {
    halted = halt;
}

void wait(uint32_t millis) {
    uint32_t countdown = millis * 1000;
    while (countdown > 0) {
        uint32_t countdown = countdown - 1;
    }
    
}

void waitSecs(int secs) {
	uint32_t end = end * 1000;
	wait(end);
}

bool countstate = false;
int time2 = 0;

// New count feature cuz why not?
// They are all counted in seconds
void Count() {
	while (countstate == true) {
		waitSecs(1);
		time2++;
	}
}

// get time from the new count feature
int getTime() {
	return time2;
}

// Sets time in seconds
int setTime(int sec) {
	time2 = sec;
}


void disableCount() {
	countstate = false;
}

void enableCount() {
	countstate = true;
}

void haltLoop() {
    while (true) {
        if (halted == 0) {
            break;
        }
        if (halted == 1) {
            continue;
        }
    }
}