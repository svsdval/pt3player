#ifndef LOAD_TEXT_H
#define LOAD_TEXT_H

struct text_parser {
    char* text;
    int size;
    int index;
};

struct ay_data {
    int sample_rate;
    int is_ym;
    int clock_rate;
    double frame_rate;
    double pan[3];
    int eqp_stereo_on;
    int dc_filter_on;
    int note_table;
};

int load_text_file(const char* name, struct ay_data* t);
int parse_int(struct text_parser* p, int* n);
int parse_float(struct text_parser* p, double* n);

#endif