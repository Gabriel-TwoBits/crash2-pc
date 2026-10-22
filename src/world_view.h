#ifndef WORLD_VIEW_H
#define WORLD_VIEW_H

/* The world beside the camera's view at 16:9 (world_view.c) */

/* At a game frame's end: the frame buffers made the host's larger ones when 16:9 is chosen while a level is open */
void WorldView_FrameEnded(void);

#endif /* WORLD_VIEW_H */
