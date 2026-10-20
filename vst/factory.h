/* Factory sounds for the first preset slots: "key=value;" of the sound parameters (keys left
 * out stay at start_values()). VOLUME evens them out at about -18 dBFS RMS for a guitar peaking
 * around -12 dBFS (host_test.cpp "levels"); the INPUT knob is the project's, not the preset's. */
static const struct { const char *name, *state; } FACTORY[] = {
    {"CLEAN TWIN", "model=5;pregain=0;master=12;drive=5;dist=10;stack=2;cab=6;bass=45;middle=50;treble=60;presence=40;rev_mix=20;rev_room=45;volume=6.1;"},
    {"GOA CLEAN DLY", "model=1;pregain=0;master=10;drive=10;dist=20;stack=23;cab=1;presence=50;treble=58;dly_time=2;dly_mix=35;dly_fb=40;dly_tone=60;dly_pp=1;rev_mix=20;volume=2;"},
    {"CHIME AC", "model=14;master=4;drive=30;dist=40;stack=8;cab=9;treble=65;presence=60;rev_mix=15;volume=3.1;"},
    {"CLASSIC ROCK", "model=10;pregain=3;drive=55;dist=60;stack=4;cab=0;bass=55;middle=65;treble=60;presence=55;rev_mix=12;volume=-6.5;"},
    {"PLEXI CRUNCH", "master=-10;model=6;pregain=6;drive=65;dist=70;stack=11;cab=8;middle=70;treble=58;rev_mix=10;volume=-4.7;"},
    {"BRIT GARAGE", "master=-12;model=14;pregain=4;drive=60;dist=70;stack=12;cab=9;middle=60;treble=60;pedal=1;p_drive=30;p_tone=55;p_level=60;rev_mix=10;volume=-10;"},
    {"60s FUZZ", "model=5;drive=20;dist=30;stack=1;cab=4;pedal=2;p_drive=85;p_level=55;treble=55;rev_mix=15;volume=0.9;"},
    {"STONER MUFF", "model=17;drive=45;dist=60;stack=18;cab=0;bass=65;middle=35;treble=50;deep=1;pedal=3;p_drive=75;p_tone=40;p_level=60;rev_mix=10;volume=-4.2;"},
    {"DOOM", "master=-12;model=4;drive=60;dist=60;stack=16;cab=12;bass=70;middle=45;treble=40;deep=1;pedal=3;p_drive=90;p_tone=30;p_level=55;rev_mix=25;rev_room=70;volume=-10.9;"},
    {"DESERT FUZZ", "model=13;drive=50;dist=60;stack=18;cab=13;deep=1;pedal=2;p_drive=70;p_level=50;dly_time=0;dly_mix=15;dly_fb=25;rev_mix=12;volume=-7.2;"},
    {"GOA LEAD", "master=-12;model=9;pregain=8;drive=80;dist=80;stack=10;cab=13;middle=60;treble=55;presence=60;pedal=1;p_drive=40;p_tone=60;p_level=55;dly_time=2;dly_mix=30;dly_fb=35;dly_pp=1;rev_mix=25;rev_room=60;volume=-8.8;"},
    {"PSY CHUG", "master=-10;model=0;pregain=10;drive=85;dist=90;stack=26;cab=5;bass=45;middle=40;treble=60;presence=65;pedal=1;p_drive=15;p_tone=70;p_level=70;rev_mix=5;volume=-4.9;"},
    {"RAT GRIND", "model=11;drive=35;dist=50;stack=13;cab=0;pedal=4;p_drive=55;p_tone=45;p_level=55;rev_mix=10;volume=0.4;"},
    {"AMBIENT SWELL", "model=2;master=8;drive=10;dist=20;stack=23;cab=1;treble=55;dly_time=0;dly_mix=45;dly_fb=65;dly_tone=40;dly_pp=1;rev_mix=60;rev_room=85;rev_damp=40;volume=3.8;"},
    {"SURF SPRING", "model=5;master=10;drive=15;dist=20;stack=2;cab=6;treble=70;presence=55;rev_mix=55;rev_room=40;rev_damp=10;volume=4.7;"},
    {"FUZZ LEAD DLY", "model=14;drive=40;dist=50;stack=4;cab=0;pedal=3;p_drive=60;p_tone=60;p_level=55;dly_time=2;dly_mix=30;dly_fb=30;rev_mix=20;volume=-0.8;"},
    {"RIFFBOX CRUNCH", "model=12;pregain=0;master=4;drive=40;dist=50;stack=5;cab=1;presence=55;rev_mix=10;volume=-1.9;"},
    {"BASS AMP", "model=5;drive=15;dist=20;stack=16;cab=12;bass=60;middle=55;treble=45;deep=1;presence=30;rev_mix=0;volume=-0.6;"},
};
