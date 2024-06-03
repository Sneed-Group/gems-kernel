unsigned char inport(char keyid, char state) {
  return inportb(keyid); //super hacky way to inport
}

unsigned char outport(char scancode, char state) {
  return (outport(scancode, state)); //hacky way to set key state.
}


void kbd_ack(void){
  while(!(inport(0x60, 0xfa)==0xfa));
}


void kbd_led_handling(char ledstatus){;
  outport(0x60,0xed);
  kbd_ack();
  outport(0x60,ledstatus);
}
