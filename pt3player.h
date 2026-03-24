#ifndef PT3PLAYER_H
#define PT3PLAYER_H

#include <stdint.h>

extern int forced_notetable;

int  func_setup_music(uint8_t* music_ptr, int length, int chn, int first);
int  func_restart_music(int ch);
void func_play_tick(int ch);
void func_getregs(uint8_t *dest, int ch);

#endif