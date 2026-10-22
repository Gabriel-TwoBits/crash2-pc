#ifndef VIDEO_H
#define VIDEO_H

/* The display at a vertical blank (video.c) */
void Host_PresentFrame(void);
/* The window as the settings have it (settings.h): full screen, the picture's shape and smoothing */
void Host_ApplySettings(void);

#endif /* VIDEO_H */
