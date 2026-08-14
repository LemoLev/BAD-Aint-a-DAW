#include "raylib.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SAMPLE_RATE 48000
#define BUFFER_SIZE 1024
#define CHANNELS 1
#define INST_CHANNELS 4
#define RECORD_PRECISION 128

typedef struct {
    float att;
    float dec;
    float sus;
    float rel;
} Envelope;

typedef struct {
    int framestamp;
    int chan;
    float v;
    Envelope env;
    float semitone;
    bool playing;
} Note;

typedef struct {
    int framestamp;
    int chan;
    float v;
    float duration;    // how long has this note been playing for (samples)
    float rels;        // RELease Seconds
    float semitone;
    bool playing;
} NoteRelease;

typedef struct {
    int chan;
    bool type;
    bool active;
} NoteRec;

float *custom;
float *metronome;
int customLength = -1;
int mtrLength = -1;
int curchan = 0;

float semitone_to_freq(float st){
    return (440.0f*powf(2.f, ((st-81.f)/12.f)));
}

float sine(int t, float semitone) {
    return sinf(2.0f * PI * semitone_to_freq(semitone) * t / SAMPLE_RATE);
}

float square(int t, float semitone) {
    float s = sinf(2.0f * PI * semitone_to_freq(semitone)*t/SAMPLE_RATE);
    return s >= 0 ? 1.f : -1.f;
}

float sawtooth(int t, float semitone) {
    return fmod((float)t/SAMPLE_RATE*semitone_to_freq(semitone+12), 2.)-1.f;
}

float noise(int t, float semitone) {
    srand(t*semitone);
    return (float)(rand() % 32767)/16383.f-1.f;
}

float get_metronome_sample(float c) {
    if (c >= mtrLength)
       return 0.f;
    return metronome[(int)floorf(c)];
}

float get_custom_sample(float c) {
    if (c >= customLength)
       return 0.f;
    return custom[(int)floorf(c)];
}

float render(float t, float semitone, int chan) {
    switch (chan) {
        case 0:
            return get_custom_sample(t*powf(2.f, (semitone-72)/12));
        case 1:
            return sine(t, semitone+12)*0.08f;
        case 2:
            return (
                sine(t, semitone+24.f)*0.6f
                +(square(t, semitone+12.07f)*0.75f
                +square(t, semitone-11.97f)*0.75f
                +square(t, semitone+12.f)
                )*0.5f
                +(sawtooth(t, semitone+.05f)
                +sawtooth(t, semitone-.05f)
                +sawtooth(t, semitone+.1f)
                +sawtooth(t, semitone-.1f)
                )*0.76f
            )*0.08f;
        case 3:
            return noise(t, semitone)*0.2f;
        default:
            return 0;
    }
}

float render_note(int t, Note* n){
    if (!n->playing) return 0;
    n->v = fminf(1.f, ((float)t-(float)n->framestamp)/(n->env.att*SAMPLE_RATE)); // attack
    n->v *= 1.f + fminf(1.f, ((float)t-(float)n->framestamp-n->env.att)/(n->env.dec*SAMPLE_RATE))*(n->env.sus-1.f); // decay/sustain
    return render((float)t-(float)n->framestamp, n->semitone, n->chan)*n->v;
    // return sine(t, n->semitone);
}

float render_release(int t, NoteRelease* n){
    if (!n->playing) return 0;

    float vol = 1.f-fminf(1.f, ((float)t-(float)n->framestamp-n->duration)/(n->rels*SAMPLE_RATE));
    if (vol > 0.f)
        return render((float)t-(float)n->framestamp, n->semitone, n->chan)*vol*n->v;
    n->playing = false;
    return 0;
}

float render_metronome(float t, float semitone) {
    return get_metronome_sample(t*powf(2.f, (semitone-60)/12));
}

void reloadCustom(){
    UnloadWaveSamples(custom);
    Wave customw = LoadWave("custom.wav");
    WaveFormat(&customw, SAMPLE_RATE, 32, CHANNELS);
    customLength = customw.frameCount/CHANNELS;
    custom = LoadWaveSamples(customw);
    UnloadWave(customw);
}

float transpos = 0;
float bpm = 85.f;
float stlen = 0;
int cursor = 0;
int prevcursor = 0;
int measure = 0;
bool record = false;
bool pause = false;
bool mtr_on = true;
bool notes_down[32*INST_CHANNELS] = {0};
Envelope env = {0.001f, 0.001f, 1.f, .015f};
Note notes_tf[32*INST_CHANNELS] = {0};
NoteRelease notesR_tf[32*INST_CHANNELS] = {0};
Note pbnotes_tf[32*INST_CHANNELS] = {0};
NoteRelease pbnotesR_tf[32*INST_CHANNELS] = {0};
// Pattern pattern = {0};
// NoteRec notes_pattern[128] = {0};
NoteRec notes_pattern[RECORD_PRECISION*2*32*INST_CHANNELS] = {0};
int keyboard_notes[32] = {KEY_Z, KEY_S,    KEY_X, KEY_D,     KEY_C, KEY_V, KEY_G,    KEY_B, KEY_H,   KEY_N, KEY_J,     KEY_M, // 4
                          KEY_Q, KEY_TWO,  KEY_W, KEY_THREE, KEY_E, KEY_R, KEY_FIVE, KEY_T, KEY_SIX, KEY_Y, KEY_SEVEN, KEY_U, // 5
                          KEY_I, KEY_NINE, KEY_O, KEY_ZERO,  KEY_P, 91,    61,       93};                                     // 6

void stopNote(int t, int key, int chan) {
    Note n = notes_tf[chan*32+key];
    notesR_tf[chan*32+key] = (NoteRelease){n.framestamp, n.chan, n.v, (float)(t-n.framestamp), n.env.rel, n.semitone, true};
    notes_tf[chan*32+key].playing = false;
}

void playNote(int t, int key, int chan) {
    notes_tf[chan*32+key] = (Note){t, chan, 0.f, env, key+48+transpos, true};
}

void stopRecordedNote(int t, int key, int chan) {
    Note n = pbnotes_tf[chan*32+key];
    pbnotesR_tf[chan*32+key] = (NoteRelease){n.framestamp, n.chan, n.v, (float)(t-n.framestamp), n.env.rel, n.semitone, true};
    pbnotes_tf[chan*32+key].playing = false;
}

void playRecordedNote(int t, int key, int chan) {
    pbnotes_tf[chan*32+key] = (Note){t, chan, 0.f, env, key+48+transpos, true};
}

int main(void) {
    stlen = (240.f/bpm)*SAMPLE_RATE/RECORD_PRECISION;
    InitWindow(1200, 800, "BAD Ain't a DAW");
    InitAudioDevice();
    SetAudioStreamBufferSizeDefault(BUFFER_SIZE);
    Wave customw = LoadWave("custom.wav");
    WaveFormat(&customw, SAMPLE_RATE, 32, CHANNELS);
    customLength = customw.frameCount/CHANNELS;
    custom = LoadWaveSamples(customw);
    UnloadWave(customw);

    Wave mtrnmw = LoadWave("metronome.wav");
    WaveFormat(&mtrnmw, SAMPLE_RATE, 32, CHANNELS);
    mtrLength = mtrnmw.frameCount/CHANNELS;
    metronome = LoadWaveSamples(mtrnmw);
    UnloadWave(mtrnmw);

    AudioStream stream = LoadAudioStream(SAMPLE_RATE, 32, CHANNELS);
    PlayAudioStream(stream);

    float buffer[BUFFER_SIZE*CHANNELS] = {0};
    int t = 0;

    SetTargetFPS(60);
    while (!WindowShouldClose()) {
        cursor = (int)floorf(t/stlen)%(RECORD_PRECISION*2);
        if(IsKeyPressed(KEY_TAB)) {
            record = !record;
            memset(pbnotes_tf, 0, sizeof(pbnotes_tf));
            memset(pbnotesR_tf, 0, sizeof(pbnotesR_tf));
            memset(notes_down, 0, sizeof(notes_down));

            if (!record) {
                for (int i = 0; i < 32; i++){
                    if (notes_tf[i].playing) {
                        notes_pattern[cursor*32+i] = (NoteRec){notes_tf[i].chan, false, true};
                    }
                }
            }
        }

        if(IsKeyPressed(KEY_SPACE)) {
            pause = !pause;
        }

        if (IsKeyPressed(KEY_LEFT)) {
            memset(notes_tf, 0, sizeof(notes_tf));
            memset(notesR_tf, 0, sizeof(notesR_tf));
            memset(pbnotes_tf, 0, sizeof(pbnotes_tf));
            memset(pbnotesR_tf, 0, sizeof(pbnotesR_tf));
            memset(notes_down, 0, sizeof(notes_down));
            t = 0;
            measure = 0;
        }

        if(IsKeyDown(KEY_LEFT_CONTROL)){
            if(IsKeyPressed(KEY_R)) {
                reloadCustom();
            }
            else if (IsKeyPressed(KEY_C)) {
                memset(pbnotes_tf, 0, sizeof(pbnotes_tf));
                memset(pbnotesR_tf, 0, sizeof(pbnotesR_tf));
                memset(notes_down, 0, sizeof(notes_down));
                for (int i = 0; i < RECORD_PRECISION*2*32*INST_CHANNELS; i++) {
                    if (notes_pattern[i].chan == curchan) {
                        notes_pattern[i] = (NoteRec){0};
                    }
                }
            }
            else if (IsKeyPressed(KEY_MINUS)) {
                bpm -= 1.f;
                stlen = (240.f/bpm)*SAMPLE_RATE/RECORD_PRECISION;
                memset(notes_tf, 0, sizeof(notes_tf));
                memset(notesR_tf, 0, sizeof(notesR_tf));
                memset(pbnotes_tf, 0, sizeof(pbnotes_tf));
                memset(pbnotesR_tf, 0, sizeof(pbnotesR_tf));
                memset(notes_down, 0, sizeof(notes_down));
                t = 0;
                measure = 0;
                memset(buffer, 0, sizeof(buffer));
            }
            else if (IsKeyPressed(61)) {
                bpm += 1.f;
                stlen = (240.f/bpm)*SAMPLE_RATE/RECORD_PRECISION;
                memset(notes_tf, 0, sizeof(notes_tf));
                memset(notesR_tf, 0, sizeof(notesR_tf));
                memset(pbnotes_tf, 0, sizeof(pbnotes_tf));
                memset(pbnotesR_tf, 0, sizeof(pbnotesR_tf));
                memset(notes_down, 0, sizeof(notes_down));
                t = 0;
                measure = 0;
                memset(buffer, 0, sizeof(buffer));
            }
            else if (IsKeyPressed(KEY_M)) {
                mtr_on = !mtr_on;
            }
            else if(IsKeyPressed(KEY_DOWN)){
                transpos -= 12.f;
            }
            else if (IsKeyPressed(KEY_UP)){
                transpos += 12.f;
            }
        }
        else if (IsKeyDown(KEY_LEFT_SHIFT)){
            if(IsKeyPressed(KEY_DOWN)){
                transpos -= .1f;
            }
            else if (IsKeyPressed(KEY_UP)){
                transpos += .1f;
            }
            else if (IsKeyPressed(KEY_MINUS)) {
                bpm -= 10.f;
                stlen = (240.f/bpm)*SAMPLE_RATE/RECORD_PRECISION;
                memset(notes_tf, 0, sizeof(notes_tf));
                memset(notesR_tf, 0, sizeof(notesR_tf));
                memset(pbnotes_tf, 0, sizeof(pbnotes_tf));
                memset(pbnotesR_tf, 0, sizeof(pbnotesR_tf));
                memset(notes_down, 0, sizeof(notes_down));
                t = 0;
                measure = 0;
                // memset(buffer, 0, sizeof(buffer));
            }
            else if (IsKeyPressed(61)) {
                bpm += 10.f;
                stlen = (240.f/bpm)*SAMPLE_RATE/RECORD_PRECISION;
                memset(notes_tf, 0, sizeof(notes_tf));
                memset(notesR_tf, 0, sizeof(notesR_tf));
                memset(pbnotes_tf, 0, sizeof(pbnotes_tf));
                memset(pbnotesR_tf, 0, sizeof(pbnotesR_tf));
                memset(notes_down, 0, sizeof(notes_down));
                t = 0;
                measure = 0;
                // memset(buffer, 0, sizeof(buffer));
            }
        }
        else {
            if(IsKeyPressed(KEY_DOWN)){
                transpos -= 1.f;
            }
            else if (IsKeyPressed(KEY_KP_1)){
                memset(notes_tf, 0, sizeof(notes_tf));
                memset(notesR_tf, 0, sizeof(notesR_tf));
                curchan = 0;
            }
            else if (IsKeyPressed(KEY_KP_2)){
                memset(notes_tf, 0, sizeof(notes_tf));
                memset(notesR_tf, 0, sizeof(notesR_tf));
                curchan = 1;
            }
            else if (IsKeyPressed(KEY_KP_3)){
                memset(notes_tf, 0, sizeof(notes_tf));
                memset(notesR_tf, 0, sizeof(notesR_tf));
                curchan = 2;
            }
            else if (IsKeyPressed(KEY_KP_4)){
                memset(notes_tf, 0, sizeof(notes_tf));
                memset(notesR_tf, 0, sizeof(notesR_tf));
                curchan = 3;
            }
            else if (IsKeyPressed(KEY_UP)){
                transpos += 1.f;
            }
            for (int i = 0; i < 32; i++){
                if (!notes_tf[curchan*32+i].playing || !IsKeyDown(keyboard_notes[i])) {
                    if (IsKeyDown(keyboard_notes[i])) {
                        playNote(t, i, curchan);
                        if (record) {
                            notes_pattern[curchan*RECORD_PRECISION*2*32+cursor*32+i] = (NoteRec){curchan, true, true};
                        }
                    }
                    else
                        if (notes_tf[curchan*32+i].playing) {
                            stopNote(t, i, curchan);
                            if (record)
                                notes_pattern[curchan*RECORD_PRECISION*2*32+cursor*32+i] = (NoteRec){curchan, false, true};
                        }
                }
            }
        }
        if (!record)
            for (int ch = 0; ch < 4; ch++) {
                for (int i = 0; i < 32; i++) {
                    for (int c = prevcursor; c <= (cursor >= prevcursor ? cursor : prevcursor+cursor); c++) {
                        NoteRec np = notes_pattern[ch*RECORD_PRECISION*2*32+(c%(RECORD_PRECISION*2))*32+i];
                        if (np.active) {
                            notes_down[np.chan*32+i] = np.type;
                        }
                        if (!pbnotes_tf[np.chan*32+i].playing || !notes_down[np.chan*32+i]) {
                            if (notes_down[np.chan*32+i]) {
                                playRecordedNote(t, i, np.chan);
                            }
                            else
                                if (pbnotes_tf[np.chan*32+i].playing) {
                                    stopRecordedNote(t, i, np.chan);
                                }
                        }
                    }
                }
            }
        if (IsAudioStreamProcessed(stream)) {
            // buffer = {0};
            for (int i = 0; i < BUFFER_SIZE*CHANNELS; i++) {
                buffer[i] = 0;
                if (!pause) {
                    for (int j = 0; j < 32*INST_CHANNELS; j++){
                        // if(notes_tf[j].playing)
                        //     buffer[i] += render(t, j);
                        buffer[i] += render_note(t, &notes_tf[j]);
                        buffer[i] += render_note(t, &pbnotes_tf[j]);
                        buffer[i] += render_release(t, &notesR_tf[j]);
                        buffer[i] += render_release(t, &pbnotesR_tf[j]);
                    }
                    if (t%(int)(60.f/bpm*SAMPLE_RATE) == 0) {
                        measure %= 4;
                        measure++;
                    }
                    if (mtr_on)
                        buffer[i] += render_metronome(t%(int)(60.f/bpm*SAMPLE_RATE), (measure == 1) ? 67 : 60);
                    t++;
                }

                // t %= (int)stlen*RECORD_PRECISION*2;
            }
            UpdateAudioStream(stream, buffer, BUFFER_SIZE);
        }

        BeginDrawing();
        ClearBackground(BLACK);
        DrawRectangle(0,    0, 5, 800, (Color){20, 20, 20, 255});
        DrawRectangle(150,  0, 5, 800, (Color){10, 10, 10, 255});
        DrawRectangle(300,  0, 5, 800, (Color){10, 10, 10, 255});
        DrawRectangle(450,  0, 5, 800, (Color){10, 10, 10, 255});
        DrawRectangle(600,  0, 5, 800, (Color){20, 20, 20, 255});
        DrawRectangle(750,  0, 5, 800, (Color){10, 10, 10, 255});
        DrawRectangle(900,  0, 5, 800, (Color){10, 10, 10, 255});
        DrawRectangle(1050, 0, 5, 800, (Color){10, 10, 10, 255});
        for (int ch = 0; ch < 4; ch++) {
            for (int c = 0; c < RECORD_PRECISION*2; c++) {
                for (int i = 0; i < 32; i++) {
                    if (notes_pattern[ch*RECORD_PRECISION*2*32+c*32+i].active)
                        switch (notes_pattern[ch*RECORD_PRECISION*2*32+c*32+i].chan) {
                            case 0:
                                DrawRectangle((1200.f/RECORD_PRECISION/2)*c, 21*(32-i), 1200.f/RECORD_PRECISION/2, 21, notes_pattern[ch*RECORD_PRECISION*2*32+c*32+i].type ? (Color){255, 255, 0, 128} : (Color){128, 128, 0, 128});
                                break;
                            case 1:
                                DrawRectangle((1200.f/RECORD_PRECISION/2)*c, 21*(32-i), 1200.f/RECORD_PRECISION/2, 21, notes_pattern[ch*RECORD_PRECISION*2*32+c*32+i].type ? (Color){0, 0, 255, 128} : (Color){0, 0, 128, 128});
                                break;
                            case 2:
                                DrawRectangle((1200.f/RECORD_PRECISION/2)*c, 21*(32-i), 1200.f/RECORD_PRECISION/2, 21, notes_pattern[ch*RECORD_PRECISION*2*32+c*32+i].type ? (Color){0, 255, 0, 128} : (Color){0, 128, 0, 128});
                                break;
                            case 3:
                                DrawRectangle((1200.f/RECORD_PRECISION/2)*c, 21*(32-i), 1200.f/RECORD_PRECISION/2, 21, notes_pattern[ch*RECORD_PRECISION*2*32+c*32+i].type ? (Color){255, 128, 0, 128} : (Color){128, 64, 0, 128});
                                break;
                            default:
                                DrawRectangle((1200.f/RECORD_PRECISION/2)*c, 21*(32-i), 1200.f/RECORD_PRECISION/2, 21, notes_pattern[ch*RECORD_PRECISION*2*32+c*32+i].type ? (Color){128, 128, 128, 128} : (Color){64, 64, 64, 128});
                                break;
                        }
                }
            }
        }
        for (int c = prevcursor; c <= (cursor >= prevcursor ? cursor : prevcursor+cursor); c++) {
            DrawRectangle((1200.f/RECORD_PRECISION/2)*(c%(RECORD_PRECISION*2)), 0, 1200.f/RECORD_PRECISION/2, 800, (int)((1200.f/RECORD_PRECISION/2)*(c%(RECORD_PRECISION*2)))%150 == 0 ? DARKGRAY : (Color){19, 19, 19, 128});
        }
        for (int j = 0; j < 32; j++){
            switch (j % 12) {
                case 0:
                    DrawRectangle(((j-j%12)/12*7)*63, 700, 62, 25, notes_tf[j].playing    ? YELLOW   : (pbnotes_tf[j].playing    ? BLUE    : WHITE));
                    DrawRectangle(((j-j%12)/12*7)*63, 725, 62, 25, notes_tf[32+j].playing ? BLUE     : (pbnotes_tf[32+j].playing ? YELLOW  : WHITE));
                    DrawRectangle(((j-j%12)/12*7)*63, 750, 62, 25, notes_tf[64+j].playing ? GREEN    : (pbnotes_tf[64+j].playing ? MAGENTA : WHITE));
                    DrawRectangle(((j-j%12)/12*7)*63, 775, 62, 25, notes_tf[96+j].playing ? ORANGE   : (pbnotes_tf[96+j].playing ? LIME    : WHITE));
                    break;
                case 2:
                    DrawRectangle((1+(j-j%12)/12*7)*63, 700, 62, 25, notes_tf[j].playing    ? YELLOW   : (pbnotes_tf[j].playing    ? BLUE    : WHITE));
                    DrawRectangle((1+(j-j%12)/12*7)*63, 725, 62, 25, notes_tf[32+j].playing ? BLUE     : (pbnotes_tf[32+j].playing ? YELLOW  : WHITE));
                    DrawRectangle((1+(j-j%12)/12*7)*63, 750, 62, 25, notes_tf[64+j].playing ? GREEN    : (pbnotes_tf[64+j].playing ? MAGENTA : WHITE));
                    DrawRectangle((1+(j-j%12)/12*7)*63, 775, 62, 25, notes_tf[96+j].playing ? ORANGE   : (pbnotes_tf[96+j].playing ? LIME    : WHITE));
                    break;
                case 4:
                    DrawRectangle((2+(j-j%12)/12*7)*63, 700, 62, 25, notes_tf[j].playing    ? YELLOW   : (pbnotes_tf[j].playing    ? BLUE    : WHITE));
                    DrawRectangle((2+(j-j%12)/12*7)*63, 725, 62, 25, notes_tf[32+j].playing ? BLUE     : (pbnotes_tf[32+j].playing ? YELLOW  : WHITE));
                    DrawRectangle((2+(j-j%12)/12*7)*63, 750, 62, 25, notes_tf[64+j].playing ? GREEN    : (pbnotes_tf[64+j].playing ? MAGENTA : WHITE));
                    DrawRectangle((2+(j-j%12)/12*7)*63, 775, 62, 25, notes_tf[96+j].playing ? ORANGE   : (pbnotes_tf[96+j].playing ? LIME    : WHITE));
                    break;
                case 5:
                    DrawRectangle((3+(j-j%12)/12*7)*63, 700, 62, 25, notes_tf[j].playing    ? YELLOW   : (pbnotes_tf[j].playing    ? BLUE    : WHITE));
                    DrawRectangle((3+(j-j%12)/12*7)*63, 725, 62, 25, notes_tf[32+j].playing ? BLUE     : (pbnotes_tf[32+j].playing ? YELLOW  : WHITE));
                    DrawRectangle((3+(j-j%12)/12*7)*63, 750, 62, 25, notes_tf[64+j].playing ? GREEN    : (pbnotes_tf[64+j].playing ? MAGENTA : WHITE));
                    DrawRectangle((3+(j-j%12)/12*7)*63, 775, 62, 25, notes_tf[96+j].playing ? ORANGE   : (pbnotes_tf[96+j].playing ? LIME    : WHITE));
                    break;
                case 7:
                    DrawRectangle((4+(j-j%12)/12*7)*63, 700, 62, 25, notes_tf[j].playing    ? YELLOW   : (pbnotes_tf[j].playing    ? BLUE    : WHITE));
                    DrawRectangle((4+(j-j%12)/12*7)*63, 725, 62, 25, notes_tf[32+j].playing ? BLUE     : (pbnotes_tf[32+j].playing ? YELLOW  : WHITE));
                    DrawRectangle((4+(j-j%12)/12*7)*63, 750, 62, 25, notes_tf[64+j].playing ? GREEN    : (pbnotes_tf[64+j].playing ? MAGENTA : WHITE));
                    DrawRectangle((4+(j-j%12)/12*7)*63, 775, 62, 25, notes_tf[96+j].playing ? ORANGE   : (pbnotes_tf[96+j].playing ? LIME    : WHITE));
                    break;
                case 9:
                    DrawRectangle((5+(j-j%12)/12*7)*63, 700, 62, 25, notes_tf[j].playing    ? YELLOW   : (pbnotes_tf[j].playing    ? BLUE    : WHITE));
                    DrawRectangle((5+(j-j%12)/12*7)*63, 725, 62, 25, notes_tf[32+j].playing ? BLUE     : (pbnotes_tf[32+j].playing ? YELLOW  : WHITE));
                    DrawRectangle((5+(j-j%12)/12*7)*63, 750, 62, 25, notes_tf[64+j].playing ? GREEN    : (pbnotes_tf[64+j].playing ? MAGENTA : WHITE));
                    DrawRectangle((5+(j-j%12)/12*7)*63, 775, 62, 25, notes_tf[96+j].playing ? ORANGE   : (pbnotes_tf[96+j].playing ? LIME    : WHITE));
                    break;
                case 11:
                    DrawRectangle((6+(j-j%12)/12*7)*63, 700, 62, 25, notes_tf[j].playing    ? YELLOW   : (pbnotes_tf[j].playing    ? BLUE    : WHITE));
                    DrawRectangle((6+(j-j%12)/12*7)*63, 725, 62, 25, notes_tf[32+j].playing ? BLUE     : (pbnotes_tf[32+j].playing ? YELLOW  : WHITE));
                    DrawRectangle((6+(j-j%12)/12*7)*63, 750, 62, 25, notes_tf[64+j].playing ? GREEN    : (pbnotes_tf[64+j].playing ? MAGENTA : WHITE));
                    DrawRectangle((6+(j-j%12)/12*7)*63, 775, 62, 25, notes_tf[96+j].playing ? ORANGE   : (pbnotes_tf[96+j].playing ? LIME    : WHITE));
                    break;
            }
        }
        for (int j = 0; j < 32; j++){
            switch (j % 12) {
                case 1:
                    DrawRectangle(((j-j%12)/12*7)*63+44, 700, 39, 15, notes_tf[j].playing       ? YELLOW   : (pbnotes_tf[j].playing       ? BLUE       : DARKGRAY));
                    DrawRectangle(((j-j%12)/12*7)*63+44, 715, 39, 15, notes_tf[32+j].playing    ? BLUE     : (pbnotes_tf[32+j].playing    ? YELLOW     : DARKGRAY));
                    DrawRectangle(((j-j%12)/12*7)*63+44, 730, 39, 15, notes_tf[64+j].playing    ? GREEN    : (pbnotes_tf[64+j].playing    ? MAGENTA    : DARKGRAY));
                    DrawRectangle(((j-j%12)/12*7)*63+44, 745, 39, 15, notes_tf[96+j].playing    ? ORANGE   : (pbnotes_tf[96+j].playing    ? LIME       : DARKGRAY));
                    break;
                case 3:
                    DrawRectangle((1+(j-j%12)/12*7)*63+44, 700, 39, 15, notes_tf[j].playing       ? YELLOW   : (pbnotes_tf[j].playing       ? BLUE       : DARKGRAY));
                    DrawRectangle((1+(j-j%12)/12*7)*63+44, 715, 39, 15, notes_tf[32+j].playing    ? BLUE     : (pbnotes_tf[32+j].playing    ? YELLOW     : DARKGRAY));
                    DrawRectangle((1+(j-j%12)/12*7)*63+44, 730, 39, 15, notes_tf[64+j].playing    ? GREEN    : (pbnotes_tf[64+j].playing    ? MAGENTA    : DARKGRAY));
                    DrawRectangle((1+(j-j%12)/12*7)*63+44, 745, 39, 15, notes_tf[96+j].playing    ? ORANGE   : (pbnotes_tf[96+j].playing    ? LIME       : DARKGRAY));
                    break;
                case 6:
                    DrawRectangle((3+(j-j%12)/12*7)*63+44, 700, 39, 15, notes_tf[j].playing       ? YELLOW   : (pbnotes_tf[j].playing       ? BLUE       : DARKGRAY));
                    DrawRectangle((3+(j-j%12)/12*7)*63+44, 715, 39, 15, notes_tf[32+j].playing    ? BLUE     : (pbnotes_tf[32+j].playing    ? YELLOW     : DARKGRAY));
                    DrawRectangle((3+(j-j%12)/12*7)*63+44, 730, 39, 15, notes_tf[64+j].playing    ? GREEN    : (pbnotes_tf[64+j].playing    ? MAGENTA    : DARKGRAY));
                    DrawRectangle((3+(j-j%12)/12*7)*63+44, 745, 39, 15, notes_tf[96+j].playing    ? ORANGE   : (pbnotes_tf[96+j].playing    ? LIME       : DARKGRAY));
                    break;
                case 8:
                    DrawRectangle((4+(j-j%12)/12*7)*63+44, 700, 39, 15, notes_tf[j].playing       ? YELLOW   : (pbnotes_tf[j].playing       ? BLUE       : DARKGRAY));
                    DrawRectangle((4+(j-j%12)/12*7)*63+44, 715, 39, 15, notes_tf[32+j].playing    ? BLUE     : (pbnotes_tf[32+j].playing    ? YELLOW     : DARKGRAY));
                    DrawRectangle((4+(j-j%12)/12*7)*63+44, 730, 39, 15, notes_tf[64+j].playing    ? GREEN    : (pbnotes_tf[64+j].playing    ? MAGENTA    : DARKGRAY));
                    DrawRectangle((4+(j-j%12)/12*7)*63+44, 745, 39, 15, notes_tf[96+j].playing    ? ORANGE   : (pbnotes_tf[96+j].playing    ? LIME       : DARKGRAY));
                    break;
                case 10:
                    DrawRectangle((5+(j-j%12)/12*7)*63+44, 700, 39, 15, notes_tf[j].playing       ? YELLOW   : (pbnotes_tf[j].playing       ? BLUE       : DARKGRAY));
                    DrawRectangle((5+(j-j%12)/12*7)*63+44, 715, 39, 15, notes_tf[32+j].playing    ? BLUE     : (pbnotes_tf[32+j].playing    ? YELLOW     : DARKGRAY));
                    DrawRectangle((5+(j-j%12)/12*7)*63+44, 730, 39, 15, notes_tf[64+j].playing    ? GREEN    : (pbnotes_tf[64+j].playing    ? MAGENTA    : DARKGRAY));
                    DrawRectangle((5+(j-j%12)/12*7)*63+44, 745, 39, 15, notes_tf[96+j].playing    ? ORANGE   : (pbnotes_tf[96+j].playing    ? LIME       : DARKGRAY));
                    break;
            }
        }
        if(record)
            DrawCircle(50, 50, 20, RED);
        const char *chantext = TextFormat("Channel: %d", curchan);
        const char *bpmtext = TextFormat("BPM: %.0f", bpm);
        const char *tptext = TextFormat("Transposition: %.1f", transpos);
        DrawText(tptext, 10, 40, 24, WHITE);
        DrawText(chantext, 10, 70, 24, WHITE);
        DrawText(bpmtext, 10, 10, 24, WHITE);
        if (pause){
            DrawRectangle(0, 0, 1200, 800, (Color){128, 128, 128, 128});
            DrawText("Paused", 600-MeasureText("Paused", 96)/2, 320, 96, WHITE);
        }
        EndDrawing();
        prevcursor = cursor;
    }

    UnloadAudioStream(stream);
    UnloadWaveSamples(custom);
    UnloadWaveSamples(metronome);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
