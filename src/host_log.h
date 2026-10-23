#ifndef HOST_LOG_H
#define HOST_LOG_H

/* Messages of the host's own, on stderr; Host_Fatal ends the program with a core dump, after a message box when a
 * player can see it. Host_InHandler(1) and (0) bracket a fault's handler, where no box is shown */
__attribute__((format(printf, 1, 2))) void Host_Log(const char *format, ...);
__attribute__((noreturn, format(printf, 1, 2))) void Host_Fatal(const char *format, ...);
void Host_InHandler(int inside);
/* Whether the program runs with no window for a player (CRASH2_HEADLESS) */
int Host_Headless(void);

#endif /* HOST_LOG_H */
