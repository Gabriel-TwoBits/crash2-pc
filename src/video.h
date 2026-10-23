#ifndef VIDEO_H
#define VIDEO_H

/* The display at a vertical blank (video.c) */
void Host_PresentFrame(void);
/* The window as the settings have it (settings.h): full screen, the picture's shape and smoothing */
void Host_ApplySettings(void);
/* On the game clock in the window, a game frame's end: it lasts the two vertical blanks it counts for the game */
void Host_PaceGameFrame(void);

#endif /* VIDEO_H */
