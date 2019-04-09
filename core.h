#ifndef PLAYER_H
#define PLAYER_H

#include "core_def.h"


/* control */
void pause_audio();
void stop_audio();
void set_volume(int volume);
void seek_audio(double percent);
void seek_audio_by_sec(int sec);
int get_time_length();
int is_stopping();
double get_current_time_pos();
void free_player();
int load_file(const char *filename);
#endif // PLAYER_H
