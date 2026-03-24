# Для PulseAudio:
#gcc -O2 -Wall -DUSE_PULSE -o playpt3_pulse playpt3.c ayumi.c pt3player.c load_text.c visualizer.c $(pkg-config --cflags --libs libpulse-simple) -lm
echo making linux with pulse
gcc -O2 -DUSE_PULSE -o playpt3 playpt3.c ayumi.c pt3player.c load_text.c visualizer.c $(pkg-config --cflags --libs libpulse-simple) -lm -lpthread

# Для ALSA:
echo making linux with alsa
gcc -O2 -DUSE_ALSA -o playpt3_asla playpt3.c ayumi.c pt3player.c load_text.c visualizer.c $(pkg-config --cflags --libs alsa) -lm -lpthread

#MSVC (Visual Studio Developer Command Prompt):

#cl /O2 playpt3_win.c ayumi.c pt3player.c load_text.c winmm.lib /Fe:playpt3.exe visualizer.c
#MinGW/GCC:
#win32
echo making win32
i686-w64-mingw32-gcc -O2 -o playpt3_32.exe playpt3.c ayumi.c pt3player.c load_text.c visualizer.c -lwinmm -lm
#win64
echo making win64
x86_64-w64-mingw32-gcc -O2 -o playpt3_64.exe playpt3.c ayumi.c pt3player.c load_text.c visualizer.c -lwinmm -lm