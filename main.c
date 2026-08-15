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
    Color noteMuteOn;
    Color noteMuteOff;
    Color noteOn;
    Color noteOff;
    Color playLive;
    Color playBack;
} ChannelPalette;

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
bool *chmute;
NoteRec *notes_pattern;
ChannelPalette palette[INST_CHANNELS] = {
    { {255, 255, 0,   64}, {128, 128, 0,   64}, {255, 255, 0,   128}, {128, 128, 0,   128}, YELLOW, BLUE    },
    { {0,   0,   255, 64}, {0,   0,   128, 64}, {0,   0,   255, 128}, {0,   0,   128, 128}, BLUE,   YELLOW  },
    { {0,   255, 0,   64}, {0,   128, 0,   64}, {0,   255, 0,   128}, {0,   128, 0,   128}, GREEN,  MAGENTA },
    { {255, 128, 0,   64}, {128, 64,  0,   64}, {255, 128, 0,   128}, {128, 64,  0,   128}, ORANGE, SKYBLUE }
};

typedef struct {
    int count;
    int capacity;
    _Bool *items;
} CHMUTE;

typedef struct {
    int count;
    int capacity;
    NoteRec *items;
} NPTRN;

#define da_push(xs, x) do {                                          \
    if (xs.count >= xs.capacity) {                                   \
        if (xs.capacity == 0) xs.capacity = 256;                     \
        else xs.capacity *= 2;                                       \
        xs.items = realloc(xs.items, xs.capacity*sizeof(*xs.items)); \
    }                                                                \
    xs.items[xs.count++] = x;                                        \
} while (0)

bool loading = false;
int t = 0;
int chamt;
int plen;
CHMUTE chmute_;
NPTRN ntptrn;
float transpos = 0;
float bpm = 85.f;
float stlen = 0;
int cursor = 0;
int prevcursor = 0;
int measure = 0;
bool record = false;
bool pause = false;
bool mtr_on = true;
bool typing = false;
bool notes_down[32*INST_CHANNELS] = {0};
Envelope env = {0.001f, 0.001f, 1.f, .015f};
Note notes_tf[32*INST_CHANNELS] = {0};
NoteRelease notesR_tf[32*INST_CHANNELS] = {0};
Note pbnotes_tf[32*INST_CHANNELS] = {0};
NoteRelease pbnotesR_tf[32*INST_CHANNELS] = {0};

void loadFile(char *filename) {
    ntptrn.count = 0;
    chmute_.count = 0;
    measure = 0;
    transpos = 0;
    bpm = 0;
    FILE *file = fopen(TextFormat("%s.bad", filename), "r");
    if (file == NULL){
        printf("[ERROR] Could not open %s.bad", filename);
        return;
    }
    char line[256];
    int ln = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        ln += 1;
        int i = 1;
        int cht = 0;
        NoteRec nr = {0};
        int nrp = 0;
        while (line[i]) {
            switch (line[0]) {
                case '{':
                    if (line[i] <= 57 && line[i] >= 48) {
                        switch (nrp){
                            case 0:
                                cht *= 10;
                                cht += line[i]-48;
                                if (line[i+1] > 57 || line[i+1] < 48) {
                                    nr.chan = cht;
                                    nrp = 1;
                                }
                                break;
                            case 1:
                                nr.type = line[i]-48>0;
                                nrp = 2;
                                break;
                            case 2:
                                nr.active = line[i]-48>0;
                                da_push(ntptrn, nr);
                                nrp = 0;
                                break;
                        }
                    }
                    break;
                case '[':
                    if (line[i] <= 57 && line[i] >= 48) {
                        da_push(chmute_, (line[i]-48>0));
                    }
                    break;
                case 't':
                    if (line[i] <= 57 && line[i] >= 48) {
                        transpos *= 10;
                        transpos += line[i]-48;
                        if (line[i+1] > 57 || line[i+1] < 48) {
                            transpos /= 10;
                            if (line[1] == '-'){
                                transpos *= -1;
                            }
                        }
                    }
                    break;
                case 'b':
                    if (line[i] <= 57 && line[i] >= 48) {
                        bpm *= 10;
                        bpm += line[i]-48;
                    }
                    break;
                case 'c':
                    if (line[i] <= 57 && line[i] >= 48) {
                        chamt *= 10;
                        chamt += line[i]-48;
                    }
                    break;
                case 'p':
                    if (line[i] <= 57 && line[i] >= 48) {
                        plen *= 10;
                        plen += line[i]-48;
                    }
                    break;
            }
            i += 1;
        }
    }
    t = 0;
    stlen = (240.f/bpm)*SAMPLE_RATE/RECORD_PRECISION;
    memset(notes_tf, 0, sizeof(notes_tf));
    memset(notesR_tf, 0, sizeof(notesR_tf));
    memset(pbnotes_tf, 0, sizeof(pbnotes_tf));
    memset(pbnotesR_tf, 0, sizeof(pbnotesR_tf));
    memset(notes_down, 0, sizeof(notes_down));
    // free(chmute);
    // free(notes_pattern);
    memcpy(chmute, chmute_.items, sizeof(*chmute_.items)*chmute_.count);
    memcpy(notes_pattern, ntptrn.items, sizeof(*ntptrn.items)*ntptrn.count);
    free(chmute_.items);
    free(ntptrn.items);
    chmute_.items = NULL;
    chmute_.count = 0;
    chmute_.capacity = 0;
    ntptrn.items = NULL;
    ntptrn.count = 0;
    ntptrn.capacity = 0;
}

float semitone_to_freq(float st){
    return (440.0f*powf(2.f, ((st-81.f)/12.f)));
}

float sine(int t, float semitone) {
    return sinf(2.0f * PI * semitone_to_freq(semitone) * t / SAMPLE_RATE);
}

float square(int t, float semitone) {
    return sine(t, semitone) >= 0 ? 1.f : -1.f;
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
    if (chmute[chan]) return 0;
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
            )*0.04f;
        case 3:
            return noise(t, semitone)*0.08f;
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


char projName[16];
int namelen;

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
    notes_pattern = (NoteRec*)malloc(sizeof(NoteRec)*RECORD_PRECISION*2*32*INST_CHANNELS);
    chmute = (bool*)malloc(sizeof(bool)*INST_CHANNELS);
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

    SetTargetFPS(60);
    while (!WindowShouldClose()) {
        cursor = (int)floorf(t/stlen)%(RECORD_PRECISION*2);
        if(IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL)){
            if(IsKeyPressed(KEY_R)) {
                reloadCustom();
            }
            else if (IsKeyPressed(KEY_O)) {
                if (projName[0])
                    loadFile(projName);
            }
            else if (IsKeyPressed(KEY_S)) {
                if (projName[0]) {
                    FILE *projFile = fopen(TextFormat("%s.bad", projName), "w");

                    if (projFile == NULL) {
                        printf("[ERROR] Could not open %s.bad\n", projName);
                    }
                    else {
                        fprintf(projFile, "b%d\n", (int)bpm);
                        fprintf(projFile, "t%.0f%d\n", transpos, (int)(10*(transpos-floorf(transpos))));
                        fprintf(projFile, "c%d\n", INST_CHANNELS);
                        fprintf(projFile, "p%d\n", RECORD_PRECISION*2*32*INST_CHANNELS);
                        fprintf(projFile, "[%b", chmute[0]);
                        for (int i = 0; i < INST_CHANNELS-1; i++){
                            fprintf(projFile, ",%b", chmute[i]);
                        }
                        fprintf(projFile, "]\n");
                        for (int i = 0; i < RECORD_PRECISION*2*32*INST_CHANNELS; i++) {
                            fprintf(projFile, "{%d,%b,%b}\n", notes_pattern[i].chan, notes_pattern[i].type, notes_pattern[i].active);
                        }
                        fclose(projFile);
                    }
                }
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
        else if (IsKeyDown(KEY_LEFT_SHIFT)||IsKeyDown(KEY_RIGHT_SHIFT)){
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
        else if (!typing) {
            if(IsKeyPressed(KEY_TAB)) {
                record = !record;
                memset(pbnotes_tf, 0, sizeof(pbnotes_tf));
                memset(pbnotesR_tf, 0, sizeof(pbnotesR_tf));
                memset(notes_down, 0, sizeof(notes_down));

                if (!record) {
                    for (int i = 0; i < 32; i++){
                        if (notes_tf[curchan*32+i].playing) {
                            notes_pattern[curchan*RECORD_PRECISION*2*32+cursor*32+i] = (NoteRec){notes_tf[curchan*32+i].chan, false, true};
                        }
                    }
                }
            }

            if(IsKeyPressed(KEY_F)) {
                chmute[curchan] = !chmute[curchan];
            }
            if(IsKeyPressed(KEY_SPACE)) {
                pause = !pause;
            }
            if(IsKeyPressed(KEY_ENTER)) {
                typing = true;
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
        else {
            int typed = GetCharPressed();
            if (IsKeyPressed(KEY_BACKSPACE)) {
                if (namelen > 0)
                    projName[--namelen] = 0;
            }
            else if (IsKeyPressed(KEY_ENTER)) {
                typing = false;
            }
            else if (typed > 0)
                if (namelen < 15)
                    projName[namelen++] = typed;

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
                    NoteRec n = notes_pattern[ch*RECORD_PRECISION*2*32+c*32+i];
                    if (n.active) {
                        if (chmute[n.chan])
                            DrawRectangle((1200.f/RECORD_PRECISION/2)*c, 21*(32-i), 1200.f/RECORD_PRECISION/2, 21, n.type ? palette[n.chan].noteMuteOn : palette[n.chan].noteMuteOff);
                        else
                            DrawRectangle((1200.f/RECORD_PRECISION/2)*c, 21*(32-i), 1200.f/RECORD_PRECISION/2, 21, n.type ? palette[n.chan].noteOn : palette[n.chan].noteOff);
                    }
                }
            }
        }
        for (int c = prevcursor; c <= (cursor >= prevcursor ? cursor : prevcursor+cursor); c++) {
            DrawRectangle((1200.f/RECORD_PRECISION/2)*(c%(RECORD_PRECISION*2)), 0, 1200.f/RECORD_PRECISION/2, 800, (int)((1200.f/RECORD_PRECISION/2)*(c%(RECORD_PRECISION*2)))%150 == 0 ? DARKGRAY : (Color){19, 19, 19, 128});
        }
        for (int j = 0; j < 32; j++){
            int x = -100;
            switch (j % 12) {
                case 0:
                    x = 0;
                    break;
                case 2:
                    x = 1;
                    break;
                case 4:
                    x = 2;
                    break;
                case 5:
                    x = 3;
                    break;
                case 7:
                    x = 4;
                    break;
                case 9:
                    x = 5;
                    break;
                case 11:
                    x = 6;
                    break;
            }
            for (int c = 0; c < 4; c++){
                DrawRectangle((x+(j-j%12)/12*7)*63, 700+25*c, 62, 25, notes_tf[c*32+j].playing ? palette[c].playLive : (!chmute[c] && pbnotes_tf[c*32+j].playing ? palette[c].playBack : WHITE));
            }
        }
        for (int j = 0; j < 32; j++){
            int x = -100;
            switch (j % 12) {
                case 1:
                    x = 0;
                    break;
                case 3:
                    x = 1;
                    break;
                case 6:
                    x = 3;
                    break;
                case 8:
                    x = 4;
                    break;
                case 10:
                    x = 5;
                    break;
            }
            for (int c = 0; c < 4; c++){
                DrawRectangle((x+(j-j%12)/12*7)*63+44, 700+15*c, 39, 15, notes_tf[c*32+j].playing ? palette[c].playLive : (!chmute[c] && pbnotes_tf[c*32+j].playing ? palette[c].playBack : DARKGRAY));
            }
        }
        if(record)
            DrawCircle(50, 50, 20, RED);
        const char *chmuttext = TextFormat("Channel %d muted", curchan);
        const char *chantext = TextFormat("Channel: %d", curchan);
        const char *bpmtext = TextFormat("BPM: %.0f", bpm);
        const char *tptext = TextFormat("Transposition: %.1f", transpos);
        if (chmute[curchan])
            DrawText(chmuttext, 10, 100, 24, WHITE);
        DrawText(chantext, 10, 70, 24, WHITE);
        DrawText(bpmtext, 10, 10, 24, WHITE);
        DrawText(tptext, 10, 40, 24, WHITE);
        if (pause){
            DrawRectangle(0, 0, 1200, 800, (Color){128, 128, 128, 128});
            DrawText("Paused", 600-MeasureText("Paused", 96)/2, 320, 96, WHITE);
        }
        DrawRectangle(1000, 0, 200, 50, (Color){32, 32, 32, typing ? 196 : 100});
        DrawText(projName, 1010, 10, 24, typing ? WHITE : GRAY);
        EndDrawing();
        prevcursor = cursor;
    }
    free(notes_pattern);
    free(chmute);
    UnloadAudioStream(stream);
    UnloadWaveSamples(custom);
    UnloadWaveSamples(metronome);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
